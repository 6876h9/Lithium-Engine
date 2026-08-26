#include "world/animation_player.hpp"

#include <algorithm>
#include <cmath>

namespace {

const std::string kEmptyName;

Vector3 lerp(const Vector3& a, const Vector3& b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
}

} // namespace

AnimationPlayer::AnimationPlayer(const Skeleton* skeleton, const std::vector<AnimationClip>* clips)
    : skeleton(skeleton), clips(clips) {
    const int bone_count = skeleton ? skeleton->bone_count() : 0;
    global_transforms.assign(bone_count, Matrix4x4::identity());
    bone_matrices.assign(bone_count, Matrix4x4::identity());
    accumulated.assign(bone_count, BonePose());
    bone_written.assign(bone_count, 0);

    // The importer stores the rest pose as a matrix, but every blend below happens
    // in TRS space, so pay for the decomposition once here rather than per frame.
    rest_pose.assign(bone_count, BonePose());
    for (int i = 0; i < bone_count; ++i) {
        skeleton->bones[i].local_bind_transform.decompose_trs(
            rest_pose[i].position, rest_pose[i].rotation, rest_pose[i].scale);
    }

    if (clips) states.resize(clips->size());

    // Produce the rest pose immediately. A mesh that is loaded but not yet playing
    // still has to be drawn, and an identity palette is only correct if the bind
    // pose happens to be identity - which it is not for most rigs.
    evaluate_pose();
}

// --- State lookup ----------------------------------------------------------

AnimationPlayer::AnimationState* AnimationPlayer::state_for(int clip_index) {
    if (!valid_clip(clip_index)) return nullptr;
    if (states.size() != clips->size()) states.resize(clips->size());
    return &states[clip_index];
}

const AnimationPlayer::AnimationState* AnimationPlayer::state_for(int clip_index) const {
    if (!valid_clip(clip_index)) return nullptr;
    if (clip_index >= static_cast<int>(states.size())) return nullptr;
    return &states[clip_index];
}

const std::string& AnimationPlayer::get_clip_name(int index) const {
    if (!valid_clip(index)) return kEmptyName;
    return (*clips)[index].name;
}

int AnimationPlayer::find_clip(const std::string& clip_name) const {
    if (!clips) return -1;
    for (int i = 0; i < static_cast<int>(clips->size()); ++i) {
        if ((*clips)[i].name == clip_name) return i;
    }
    return -1;
}

float AnimationPlayer::get_clip_duration_seconds(int clip_index) const {
    if (!valid_clip(clip_index)) return 0.0f;
    return (*clips)[clip_index].get_duration_seconds();
}

float AnimationPlayer::get_duration_seconds() const {
    return get_clip_duration_seconds(current_clip);
}

void AnimationPlayer::bind_channels(int clip_index) {
    AnimationState* state = state_for(clip_index);
    if (!state) return;

    const int bone_count = skeleton ? skeleton->bone_count() : 0;
    state->channel_for_bone.assign(bone_count, nullptr);
    state->channels_bound = true;

    // Sampling walks every bone every frame; searching the clip's channel list per
    // bone would make that quadratic in the bone count.
    const AnimationClip& clip = (*clips)[clip_index];
    for (const auto& channel : clip.channels) {
        if (channel.bone_index >= 0 && channel.bone_index < bone_count) {
            state->channel_for_bone[channel.bone_index] = &channel;
        }
    }
}

// --- Playback control ------------------------------------------------------

AnimationPlayer::AnimationState* AnimationPlayer::begin_state(int clip_index, bool loop) {
    AnimationState* state = state_for(clip_index);
    if (!state) return nullptr;

    if (!state->channels_bound || state->channel_for_bone.size() != static_cast<size_t>(get_bone_count())) {
        bind_channels(clip_index);
    }

    // A clip that is already contributing keeps its cursor. Restarting it would make
    // a state machine that calls crossfade() every frame stutter on frame one of the
    // clip forever.
    if (!state->playing && state->weight <= kWeightEpsilon) {
        state->time = 0.0f;
        state->speed = default_speed;
    }
    state->looping = loop;
    state->playing = true;
    state->stop_when_faded = false;
    pose_dirty = true;
    return state;
}

void AnimationPlayer::start_fade(AnimationState& state, float target_weight, float fade_seconds, bool stop_at_zero) {
    target_weight = std::max(0.0f, std::min(1.0f, target_weight));
    if (fade_seconds <= 0.0f) {
        state.weight = target_weight;
        state.fading = false;
        if (stop_at_zero && target_weight <= kWeightEpsilon) {
            state.playing = false;
            state.weight = 0.0f;
        }
    } else {
        state.fading = true;
        state.fade_from = state.weight;
        state.fade_to = target_weight;
        state.fade_duration = fade_seconds;
        state.fade_elapsed = 0.0f;
        state.stop_when_faded = stop_at_zero && (target_weight <= kWeightEpsilon);
    }
    pose_dirty = true;
}

void AnimationPlayer::play(int clip_index, bool loop) {
    if (!valid_clip(clip_index)) {
        stop();
        return;
    }

    // A hard cut: everything else stops this instant rather than fading.
    for (int i = 0; i < static_cast<int>(states.size()); ++i) {
        if (i == clip_index) continue;
        states[i].playing = false;
        states[i].weight = 0.0f;
        states[i].fading = false;
    }

    AnimationState* state = state_for(clip_index);
    // play() is a restart even for a clip that is already running - that is what
    // distinguishes it from crossfade().
    state->time = 0.0f;
    state->speed = default_speed;
    state->weight = 1.0f;
    state->fading = false;
    state->stop_when_faded = false;
    state->looping = loop;
    state->playing = true;
    if (!state->channels_bound || state->channel_for_bone.size() != static_cast<size_t>(get_bone_count())) {
        bind_channels(clip_index);
    }

    current_clip = clip_index;
    default_looping = loop;
    pose_dirty = true;
    evaluate_pose();
}

bool AnimationPlayer::play(const std::string& clip_name, bool loop) {
    int index = find_clip(clip_name);
    if (index < 0) return false;
    play(index, loop);
    return true;
}

void AnimationPlayer::stop() {
    for (auto& state : states) {
        state.playing = false;
        state.weight = 0.0f;
        state.fading = false;
        state.time = 0.0f;
    }
    pose_dirty = true;
    evaluate_pose();
}

void AnimationPlayer::crossfade(int clip_index, float fade_seconds, bool loop) {
    AnimationState* target = begin_state(clip_index, loop);
    if (!target) return;

    const int layer = target->layer;
    // Only the same layer and the same blend mode are faded out. An additive overlay
    // on layer 0 is not an alternative to the walk cycle, and a masked upper-body
    // layer must survive a change of leg animation underneath it.
    for (int i = 0; i < static_cast<int>(states.size()); ++i) {
        if (i == clip_index) continue;
        AnimationState& other = states[i];
        if (other.layer != layer || other.mode != BlendMode::Blend) continue;
        if (!other.playing && other.weight <= kWeightEpsilon) continue;
        start_fade(other, 0.0f, fade_seconds, true);
    }

    start_fade(*target, 1.0f, fade_seconds, false);
    current_clip = clip_index;
    default_looping = loop;
    evaluate_pose();
}

bool AnimationPlayer::crossfade(const std::string& clip_name, float fade_seconds, bool loop) {
    int index = find_clip(clip_name);
    if (index < 0) return false;
    crossfade(index, fade_seconds, loop);
    return true;
}

void AnimationPlayer::blend(int clip_index, float target_weight, float fade_seconds) {
    if (!valid_clip(clip_index)) return;

    if (target_weight > kWeightEpsilon) {
        AnimationState* state = begin_state(clip_index, default_looping);
        if (!state) return;
        start_fade(*state, target_weight, fade_seconds, false);
    } else {
        AnimationState* state = state_for(clip_index);
        if (!state) return;
        start_fade(*state, 0.0f, fade_seconds, true);
    }
    evaluate_pose();
}

bool AnimationPlayer::blend(const std::string& clip_name, float target_weight, float fade_seconds) {
    int index = find_clip(clip_name);
    if (index < 0) return false;
    blend(index, target_weight, fade_seconds);
    return true;
}

void AnimationPlayer::stop_clip(int clip_index) {
    AnimationState* state = state_for(clip_index);
    if (!state) return;
    state->playing = false;
    state->weight = 0.0f;
    state->fading = false;
    state->time = 0.0f;
    pose_dirty = true;
    evaluate_pose();
}

bool AnimationPlayer::stop_clip(const std::string& clip_name) {
    int index = find_clip(clip_name);
    if (index < 0) return false;
    stop_clip(index);
    return true;
}

// --- Per-state configuration -----------------------------------------------

void AnimationPlayer::set_clip_layer(int clip_index, int layer) {
    if (AnimationState* s = state_for(clip_index)) { s->layer = layer; pose_dirty = true; }
}

int AnimationPlayer::get_clip_layer(int clip_index) const {
    const AnimationState* s = state_for(clip_index);
    return s ? s->layer : 0;
}

void AnimationPlayer::set_clip_blend_mode(int clip_index, BlendMode mode) {
    if (AnimationState* s = state_for(clip_index)) { s->mode = mode; pose_dirty = true; }
}

AnimationPlayer::BlendMode AnimationPlayer::get_clip_blend_mode(int clip_index) const {
    const AnimationState* s = state_for(clip_index);
    return s ? s->mode : BlendMode::Blend;
}

void AnimationPlayer::set_clip_weight(int clip_index, float weight) {
    AnimationState* s = state_for(clip_index);
    if (!s) return;
    s->weight = std::max(0.0f, std::min(1.0f, weight));
    s->fading = false;
    // Giving a stopped clip a weight is how a pose is held without advancing it, so
    // this must not implicitly start playback - but the channels do have to exist.
    if (s->weight > kWeightEpsilon && !s->channels_bound) bind_channels(clip_index);
    pose_dirty = true;
}

float AnimationPlayer::get_clip_weight(int clip_index) const {
    const AnimationState* s = state_for(clip_index);
    return s ? s->weight : 0.0f;
}

void AnimationPlayer::set_clip_speed(int clip_index, float speed) {
    if (AnimationState* s = state_for(clip_index)) { s->speed = speed; pose_dirty = true; }
}

float AnimationPlayer::get_clip_speed(int clip_index) const {
    const AnimationState* s = state_for(clip_index);
    return s ? s->speed : default_speed;
}

void AnimationPlayer::set_clip_time(int clip_index, float seconds) {
    if (AnimationState* s = state_for(clip_index)) {
        s->time = std::max(0.0f, seconds);
        pose_dirty = true;
    }
}

float AnimationPlayer::get_clip_time(int clip_index) const {
    const AnimationState* s = state_for(clip_index);
    return s ? s->time : 0.0f;
}

void AnimationPlayer::set_clip_looping(int clip_index, bool loop) {
    if (AnimationState* s = state_for(clip_index)) s->looping = loop;
}

bool AnimationPlayer::get_clip_looping(int clip_index) const {
    const AnimationState* s = state_for(clip_index);
    return s ? s->looping : default_looping;
}

bool AnimationPlayer::is_clip_playing(int clip_index) const {
    const AnimationState* s = state_for(clip_index);
    return s && s->playing;
}

// --- Bone masks ------------------------------------------------------------

bool AnimationPlayer::apply_bone_mask(int clip_index, const std::string& root_bone_name,
                                      bool include_descendants, bool reset_existing) {
    AnimationState* state = state_for(clip_index);
    if (!state || !skeleton) return false;

    const int root = skeleton->find_bone(root_bone_name);
    if (root < 0) return false;

    const int bone_count = skeleton->bone_count();
    if (reset_existing || static_cast<int>(state->bone_mask.size()) != bone_count) {
        state->bone_mask.assign(bone_count, 0.0f);
    }

    state->bone_mask[root] = 1.0f;
    if (include_descendants) {
        // Bones are stored parents-first, so one forward sweep propagates the whole
        // sub-tree: a bone is included exactly when its parent already is.
        for (int i = root + 1; i < bone_count; ++i) {
            const int parent = skeleton->bones[i].parent_index;
            if (parent >= 0 && parent < i && state->bone_mask[parent] > 0.0f) {
                state->bone_mask[i] = 1.0f;
            }
        }
    }

    pose_dirty = true;
    return true;
}

bool AnimationPlayer::set_clip_bone_mask(int clip_index, const std::string& root_bone_name, bool include_descendants) {
    return apply_bone_mask(clip_index, root_bone_name, include_descendants, true);
}

bool AnimationPlayer::add_clip_bone_mask(int clip_index, const std::string& root_bone_name, bool include_descendants) {
    return apply_bone_mask(clip_index, root_bone_name, include_descendants, false);
}

void AnimationPlayer::clear_clip_bone_mask(int clip_index) {
    if (AnimationState* s = state_for(clip_index)) {
        s->bone_mask.clear();
        pose_dirty = true;
    }
}

bool AnimationPlayer::has_clip_bone_mask(int clip_index) const {
    const AnimationState* s = state_for(clip_index);
    return s && !s->bone_mask.empty();
}

// --- Aggregate queries -----------------------------------------------------

int AnimationPlayer::get_active_state_count() const {
    int count = 0;
    for (const auto& state : states) {
        if (state.weight > kWeightEpsilon) ++count;
    }
    return count;
}

bool AnimationPlayer::is_playing() const {
    for (const auto& state : states) {
        if (state.playing) return true;
    }
    return false;
}

void AnimationPlayer::set_playing(bool value) {
    bool any = false;
    for (auto& state : states) {
        // Only states that are contributing can be resumed; resuming one at weight 0
        // would restart clips the user faded out minutes ago.
        if (value) {
            if (state.weight > kWeightEpsilon) { state.playing = true; any = true; }
        } else {
            state.playing = false;
        }
    }
    // Nothing was left holding a weight, so fall back to restarting the clip the
    // editor is showing - which is what pressing play on a stopped player means.
    if (value && !any && valid_clip(current_clip)) {
        AnimationState* state = state_for(current_clip);
        state->weight = 1.0f;
        state->playing = true;
        if (!state->channels_bound) bind_channels(current_clip);
    }
    pose_dirty = true;
}

bool AnimationPlayer::is_looping() const {
    const AnimationState* s = state_for(current_clip);
    return s ? s->looping : default_looping;
}

void AnimationPlayer::set_looping(bool value) {
    default_looping = value;
    if (AnimationState* s = state_for(current_clip)) s->looping = value;
}

float AnimationPlayer::get_speed() const {
    const AnimationState* s = state_for(current_clip);
    return s ? s->speed : default_speed;
}

void AnimationPlayer::set_speed(float value) {
    default_speed = value;
    if (AnimationState* s = state_for(current_clip)) { s->speed = value; pose_dirty = true; }
}

float AnimationPlayer::get_time_seconds() const {
    const AnimationState* s = state_for(current_clip);
    return s ? s->time : 0.0f;
}

void AnimationPlayer::set_time_seconds(float seconds) {
    if (AnimationState* s = state_for(current_clip)) {
        s->time = std::max(0.0f, seconds);
        pose_dirty = true;
    }
}

// --- Per-frame advance -----------------------------------------------------

void AnimationPlayer::advance_state(AnimationState& state, const AnimationClip& clip, float delta_seconds) {
    const float duration = clip.get_duration_seconds();
    if (duration <= 0.0f) return;

    state.time += delta_seconds * state.speed;
    if (state.looping) {
        // fmod rather than a subtract loop: a large delta (a stall, or a scrub) can
        // overshoot by many periods at once.
        state.time = std::fmod(state.time, duration);
        if (state.time < 0.0f) state.time += duration;
    } else if (state.time >= duration) {
        state.time = duration;
        state.playing = false;
    } else if (state.time < 0.0f) {
        state.time = 0.0f;
        state.playing = false;
    }
    pose_dirty = true;
}

void AnimationPlayer::update(float delta_seconds) {
    if (!clips) {
        if (pose_dirty) evaluate_pose();
        return;
    }
    if (states.size() != clips->size()) states.resize(clips->size());

    for (size_t i = 0; i < states.size(); ++i) {
        AnimationState& state = states[i];

        if (state.fading) {
            state.fade_elapsed += delta_seconds;
            if (state.fade_elapsed >= state.fade_duration) {
                state.weight = state.fade_to;
                state.fading = false;
                if (state.stop_when_faded) {
                    // Reaching silence ends the state rather than leaving it running
                    // at weight 0, where it would keep being advanced and sampled for
                    // a contribution nothing can see.
                    state.playing = false;
                    state.weight = 0.0f;
                    state.stop_when_faded = false;
                }
            } else {
                const float t = (state.fade_duration > 0.0f) ? (state.fade_elapsed / state.fade_duration) : 1.0f;
                state.weight = state.fade_from + (state.fade_to - state.fade_from) * t;
            }
            pose_dirty = true;
        }

        // Advanced regardless of weight. A state fading in from zero still has to
        // move, or the clip would sit on frame one until the fade crossed the
        // contribution threshold and then jump.
        if (state.playing) {
            advance_state(state, (*clips)[i], delta_seconds);
        }
    }

    if (pose_dirty) {
        evaluate_pose();
    }
}

// --- Pose evaluation -------------------------------------------------------

float AnimationPlayer::state_bone_weight(const AnimationState& state, int bone_index) const {
    if (state.bone_mask.empty()) return state.weight;
    if (bone_index < 0 || bone_index >= static_cast<int>(state.bone_mask.size())) return state.weight;
    return state.weight * state.bone_mask[bone_index];
}

AnimationPlayer::BonePose AnimationPlayer::sample_state(const PreparedState& prepared, int bone_index, float ticks) const {
    const AnimationState& state = *prepared.state;
    if (bone_index < 0 || bone_index >= static_cast<int>(state.channel_for_bone.size())) {
        return rest_pose[bone_index];
    }
    const BoneChannel* channel = state.channel_for_bone[bone_index];
    // A clip that only animates the arms still has to place the legs somewhere, and
    // the imported rest pose is the only correct answer. Collapsing them onto their
    // parent instead folds the mesh into the origin.
    if (!channel) return rest_pose[bone_index];

    BonePose pose;
    pose.position = channel->sample_position(ticks);
    pose.rotation = channel->sample_rotation(ticks);
    pose.scale    = channel->sample_scale(ticks);
    return pose;
}

void AnimationPlayer::evaluate_pose() {
    pose_dirty = false;
    if (!skeleton || skeleton->empty()) return;

    const int bone_count = skeleton->bone_count();
    if (static_cast<int>(global_transforms.size()) != bone_count) {
        global_transforms.assign(bone_count, Matrix4x4::identity());
        bone_matrices.assign(bone_count, Matrix4x4::identity());
        accumulated.assign(bone_count, BonePose());
        bone_written.assign(bone_count, 0);
    }
    if (static_cast<int>(bone_written.size()) != bone_count) bone_written.assign(bone_count, 0);

    // Resolve the states that will actually contribute, with their per-frame
    // constants computed once rather than per bone.
    prepared.clear();
    layer_order.clear();
    if (clips) {
        for (size_t i = 0; i < states.size() && i < clips->size(); ++i) {
            const AnimationState& state = states[i];
            if (state.weight <= kWeightEpsilon) continue;
            if (state.channel_for_bone.size() != static_cast<size_t>(bone_count)) continue;

            const AnimationClip& clip = (*clips)[i];
            PreparedState p;
            p.state = &state;
            p.clip = &clip;
            // Keys are timed in ticks, not seconds.
            p.ticks = state.time * clip.ticks_per_second;
            if (clip.duration > 0.0f) p.ticks = std::min(p.ticks, clip.duration);
            p.reference_ticks = 0.0f;
            prepared.push_back(p);

            if (std::find(layer_order.begin(), layer_order.end(), state.layer) == layer_order.end()) {
                layer_order.push_back(state.layer);
            }
        }
    }
    std::sort(layer_order.begin(), layer_order.end());

    // Everything blends over the rest pose, so a partially weighted single clip
    // reads as "half way from the bind pose into the animation" rather than as an
    // undefined half-transform.
    for (int i = 0; i < bone_count; ++i) {
        accumulated[i] = rest_pose[i];
        bone_written[i] = 0;
    }

    for (int layer : layer_order) {
        // --- Override states -------------------------------------------------
        // Blended together among themselves first, then the result is laid over
        // whatever the layers below produced. Doing it in that order is what makes a
        // layer at total weight 0.5 mean "half of this layer", rather than letting
        // two 0.5-weight clips in the same layer each individually dilute the base.
        for (int bone = 0; bone < bone_count; ++bone) {
            BonePose blended;
            float accumulated_weight = 0.0f;

            for (const PreparedState& p : prepared) {
                if (p.state->layer != layer || p.state->mode != BlendMode::Blend) continue;
                const float w = state_bone_weight(*p.state, bone);
                if (w <= kWeightEpsilon) continue;

                const BonePose sampled = sample_state(p, bone, p.ticks);
                if (accumulated_weight <= 0.0f) {
                    blended = sampled;
                    accumulated_weight = w;
                } else {
                    // Incremental blend: each new state takes its share of the
                    // running total, which yields the same result as a single
                    // normalised weighted average without needing two passes.
                    const float t = w / (accumulated_weight + w);
                    blended.position = lerp(blended.position, sampled.position, t);
                    blended.rotation = Vector4::quat_slerp(blended.rotation, sampled.rotation, t);
                    blended.scale    = lerp(blended.scale, sampled.scale, t);
                    accumulated_weight += w;
                }
            }

            if (accumulated_weight > kWeightEpsilon) {
                bone_written[bone] = 1;
                const float layer_weight = std::min(1.0f, accumulated_weight);
                accumulated[bone].position = lerp(accumulated[bone].position, blended.position, layer_weight);
                accumulated[bone].rotation = Vector4::quat_slerp(accumulated[bone].rotation, blended.rotation, layer_weight);
                accumulated[bone].scale    = lerp(accumulated[bone].scale, blended.scale, layer_weight);
            }
        }

        // --- Additive states -------------------------------------------------
        // Applied after the overrides on the same layer, as a difference from the
        // clip's own first frame. Taking the reference from the clip rather than
        // from the rest pose is what lets an additive clip be authored as a normal
        // animation and still layer correctly.
        for (const PreparedState& p : prepared) {
            if (p.state->layer != layer || p.state->mode != BlendMode::Additive) continue;

            for (int bone = 0; bone < bone_count; ++bone) {
                const float w = state_bone_weight(*p.state, bone);
                if (w <= kWeightEpsilon) continue;

                const BonePose sampled = sample_state(p, bone, p.ticks);
                const BonePose reference = sample_state(p, bone, p.reference_ticks);

                bone_written[bone] = 1;
                BonePose& target = accumulated[bone];
                target.position += (sampled.position - reference.position) * w;

                // delta = sampled * inverse(reference), applied on the left so it
                // rotates the accumulated pose rather than replacing it.
                const Vector4 delta = Vector4::quat_mul(sampled.rotation, Vector4::quat_conjugate(reference.rotation));
                const Vector4 rotated = Vector4::quat_mul(delta, target.rotation);
                target.rotation = Vector4::quat_slerp(target.rotation, rotated, w);

                // Scale composes multiplicatively, so the additive term is a ratio
                // and the weight interpolates between 1 (no change) and that ratio.
                const float rx = (std::abs(reference.scale.x) > 1e-6f) ? sampled.scale.x / reference.scale.x : 1.0f;
                const float ry = (std::abs(reference.scale.y) > 1e-6f) ? sampled.scale.y / reference.scale.y : 1.0f;
                const float rz = (std::abs(reference.scale.z) > 1e-6f) ? sampled.scale.z / reference.scale.z : 1.0f;
                target.scale.x *= 1.0f + (rx - 1.0f) * w;
                target.scale.y *= 1.0f + (ry - 1.0f) * w;
                target.scale.z *= 1.0f + (rz - 1.0f) * w;
            }
        }
    }

    for (int i = 0; i < bone_count; ++i) {
        const Bone& bone = skeleton->bones[i];
        // A bone nothing wrote to keeps the importer's own matrix. Recomposing it
        // from the decomposed rest pose would be a round-trip through TRS, which is
        // exact for the rigid and scaled transforms rigs actually use but has no
        // reason to be taken at all when no clip touched the bone.
        const Matrix4x4 local = bone_written[i]
            ? Matrix4x4::from_trs(accumulated[i].position, accumulated[i].rotation, accumulated[i].scale)
            : bone.local_bind_transform;

        // Bones are stored parents-first, so the parent's global is already final.
        if (bone.parent_index >= 0 && bone.parent_index < i) {
            global_transforms[i] = global_transforms[bone.parent_index] * local;
        } else {
            global_transforms[i] = local;
        }

        bone_matrices[i] = skeleton->global_inverse_transform * global_transforms[i] * bone.inverse_bind_pose;
    }
}
