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

// Out of line, and defaulted. The class holds a unique_ptr<AnimStateMachine>, so
// the destructor has to be emitted somewhere the graph type is complete rather
// than at every point a StaticMeshComponent happens to destroy a player.
AnimationPlayer::~AnimationPlayer() = default;

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

    // Foot placement runs after the pose exists, because it traces from where the
    // animation actually put each foot. It then writes IK targets, so the pose is
    // re-evaluated to apply them. Ordering it the other way round would solve the
    // legs against last frame's ground.
    if (foot_settings.enabled && ground_probe && !foot_chains.empty()) {
        update_foot_placement(delta_seconds);
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

    // IK runs on the finished FK pose: the solver needs to know where the animation
    // put each limb before it can express the correction as a delta from it.
    apply_ik();
}

// ===========================================================================
//  Inverse kinematics
//
//  Analytic two-bone solver. The chain is grandparent -> parent -> tip, and the
//  solve is two rotations: bend the joint to the angle that puts the tip at the
//  right distance, then swing the whole limb so the tip lands on the target.
//  Because it is closed-form there is no iteration, no convergence threshold and
//  no way for it to end up somewhere different on two frames with the same input.
//
//  Everything here works in mesh space - the space the vertices are in, which is
//  global_inverse_transform * global_transforms[bone]. Working in the raw node
//  space the FK pass accumulates in would put targets in whatever units and axis
//  convention the exporter happened to use.
// ===========================================================================

namespace {

Vector3 matrix_position(const Matrix4x4& m) {
    return { m.m[12], m.m[13], m.m[14] };
}

float vector_length(const Vector3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// Rotation taking `from` onto `to`, as a quaternion. Both are assumed normalised.
Vector4 rotation_between(const Vector3& from, const Vector3& to) {
    const float d = Vector3::dot(from, to);
    if (d > 0.99999f) return { 0.0f, 0.0f, 0.0f, 1.0f };
    if (d < -0.99999f) {
        // Opposed: any perpendicular axis is a valid 180-degree rotation. Picking
        // the one furthest from `from` keeps the cross product well conditioned.
        Vector3 axis = std::fabs(from.x) < 0.9f ? Vector3{ 1.0f, 0.0f, 0.0f }
                                                : Vector3{ 0.0f, 1.0f, 0.0f };
        axis = Vector3::cross(from, axis).normalized();
        return { axis.x, axis.y, axis.z, 0.0f };
    }
    const Vector3 axis = Vector3::cross(from, to);
    const float w = 1.0f + d;
    Vector4 q{ axis.x, axis.y, axis.z, w };
    return q.normalized();
}

// Rotates a direction without translating it. Matrix4x4::operator*(Vector3) is a
// point transform, so using it on a normal would add the matrix's translation.
Vector3 transform_direction(const Matrix4x4& m, const Vector3& v) {
    return { m.m[0] * v.x + m.m[4] * v.y + m.m[8]  * v.z,
             m.m[1] * v.x + m.m[5] * v.y + m.m[9]  * v.z,
             m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z };
}

Vector4 quat_from_axis_angle(const Vector3& axis, float radians) {
    const float half = radians * 0.5f;
    const float s = std::sin(half);
    return { axis.x * s, axis.y * s, axis.z * s, std::cos(half) };
}

// Applies a rotation about a pivot to a global transform:
//   new = T(pivot) * R * T(-pivot) * old
Matrix4x4 rotate_about(const Matrix4x4& global, const Vector3& pivot, const Vector4& rotation) {
    const Matrix4x4 to_origin = Matrix4x4::translation({ -pivot.x, -pivot.y, -pivot.z });
    const Matrix4x4 back = Matrix4x4::translation(pivot);
    return back * Matrix4x4::from_quaternion(rotation) * to_origin * global;
}

} // namespace

int AnimationPlayer::add_two_bone_ik(const std::string& end_bone_name) {
    if (!skeleton) return -1;
    const int end = skeleton->find_bone(end_bone_name);
    if (end < 0) return -1;
    const int mid = skeleton->bones[end].parent_index;
    if (mid < 0) return -1;
    const int root = skeleton->bones[mid].parent_index;
    // A tip without both a parent and a grandparent has no two bones to rotate.
    if (root < 0) return -1;

    IKChain chain;
    chain.end_bone = end;
    chain.mid_bone = mid;
    chain.root_bone_index = root;
    ik_chains.push_back(chain);
    return static_cast<int>(ik_chains.size()) - 1;
}

bool AnimationPlayer::set_ik_target(int handle, const Vector3& mesh_space_target) {
    if (handle < 0 || handle >= static_cast<int>(ik_chains.size())) return false;
    ik_chains[handle].target = mesh_space_target;
    ik_chains[handle].has_target = true;
    pose_dirty = true;
    return true;
}

bool AnimationPlayer::set_ik_pole(int handle, const Vector3& mesh_space_pole) {
    if (handle < 0 || handle >= static_cast<int>(ik_chains.size())) return false;
    ik_chains[handle].pole = mesh_space_pole;
    ik_chains[handle].has_pole = true;
    pose_dirty = true;
    return true;
}

bool AnimationPlayer::clear_ik_pole(int handle) {
    if (handle < 0 || handle >= static_cast<int>(ik_chains.size())) return false;
    ik_chains[handle].has_pole = false;
    pose_dirty = true;
    return true;
}

bool AnimationPlayer::set_ik_weight(int handle, float weight) {
    if (handle < 0 || handle >= static_cast<int>(ik_chains.size())) return false;
    ik_chains[handle].weight = std::clamp(weight, 0.0f, 1.0f);
    pose_dirty = true;
    return true;
}

float AnimationPlayer::get_ik_weight(int handle) const {
    if (handle < 0 || handle >= static_cast<int>(ik_chains.size())) return 0.0f;
    return ik_chains[handle].weight;
}

bool AnimationPlayer::set_ik_enabled(int handle, bool enabled) {
    if (handle < 0 || handle >= static_cast<int>(ik_chains.size())) return false;
    ik_chains[handle].enabled = enabled;
    pose_dirty = true;
    return true;
}

bool AnimationPlayer::is_ik_enabled(int handle) const {
    if (handle < 0 || handle >= static_cast<int>(ik_chains.size())) return false;
    return ik_chains[handle].enabled;
}

bool AnimationPlayer::get_ik_bones(int handle, int& out_root, int& out_mid, int& out_end) const {
    if (handle < 0 || handle >= static_cast<int>(ik_chains.size())) return false;
    const IKChain& chain = ik_chains[handle];
    out_root = chain.root_bone_index;
    out_mid = chain.mid_bone;
    out_end = chain.end_bone;
    return true;
}

bool AnimationPlayer::get_bone_mesh_transform(int bone_index, Matrix4x4& out_transform) const {
    if (!skeleton || bone_index < 0 || bone_index >= static_cast<int>(global_transforms.size())) {
        return false;
    }
    out_transform = skeleton->global_inverse_transform * global_transforms[bone_index];
    return true;
}

bool AnimationPlayer::get_bone_mesh_position(int bone_index, Vector3& out_position) const {
    Matrix4x4 transform;
    if (!get_bone_mesh_transform(bone_index, transform)) return false;
    out_position = matrix_position(transform);
    return true;
}

// Re-derives every bone at or below `first_dirty` from its parent, then rebuilds
// the skinning palette. Called after the solver has rewritten a chain's globals:
// the hand at the end of a solved arm has to follow the forearm it hangs off.
void AnimationPlayer::refresh_descendants(int first_dirty) {
    if (!skeleton) return;
    const int bone_count = skeleton->bone_count();
    for (int i = first_dirty + 1; i < bone_count; ++i) {
        if (ik_dirty[i]) continue;   // written by the solver; keep it
        const int parent = skeleton->bones[i].parent_index;
        if (parent >= 0 && parent < i) {
            // The bone's own local transform did not change - only its parent moved.
            global_transforms[i] = global_transforms[parent] * ik_local_cache[i];
        }
    }
    for (int i = 0; i < bone_count; ++i) {
        bone_matrices[i] = skeleton->global_inverse_transform * global_transforms[i] *
                           skeleton->bones[i].inverse_bind_pose;
    }
}

void AnimationPlayer::apply_ik() {
    if (!skeleton || ik_chains.empty()) return;
    const int bone_count = skeleton->bone_count();
    if (bone_count == 0) return;

    bool any_active = false;
    for (const IKChain& chain : ik_chains) {
        if (chain.enabled && chain.has_target && chain.weight > kWeightEpsilon) {
            any_active = true;
            break;
        }
    }
    if (!any_active) return;

    // Local transforms are cached before anything moves, so a bone hanging off a
    // solved chain can be rebuilt from its parent without re-running the whole FK
    // pass. Parent global inverse times child global is exactly its local.
    ik_local_cache.assign(bone_count, Matrix4x4::identity());
    ik_dirty.assign(bone_count, 0);
    for (int i = 0; i < bone_count; ++i) {
        const int parent = skeleton->bones[i].parent_index;
        ik_local_cache[i] = (parent >= 0 && parent < i)
            ? global_transforms[parent].inverse() * global_transforms[i]
            : global_transforms[i];
    }

    const Matrix4x4& to_mesh = skeleton->global_inverse_transform;
    const Matrix4x4 from_mesh = to_mesh.inverse();
    int lowest_dirty = bone_count;

    for (const IKChain& chain : ik_chains) {
        if (!chain.enabled || !chain.has_target || chain.weight <= kWeightEpsilon) continue;
        const int root = chain.root_bone_index;
        const int mid = chain.mid_bone;
        const int end = chain.end_bone;
        if (root < 0 || mid < 0 || end < 0) continue;

        // Mesh space throughout, so targets mean what the caller thinks they mean.
        Matrix4x4 root_mesh = to_mesh * global_transforms[root];
        Matrix4x4 mid_mesh = to_mesh * global_transforms[mid];
        Matrix4x4 end_mesh = to_mesh * global_transforms[end];

        const Vector3 a = matrix_position(root_mesh);
        const Vector3 b = matrix_position(mid_mesh);
        const Vector3 c = matrix_position(end_mesh);

        const float upper = vector_length(b - a);
        const float lower = vector_length(c - b);
        if (upper < 1e-5f || lower < 1e-5f) continue;

        // Out of reach is not an error: clamp the target onto the sphere of maximum
        // reach and the limb straightens toward it, rather than tearing or flipping.
        // The inner bound is the fold-back limit, where the two bones are colinear.
        Vector3 to_target = chain.target - a;
        float reach = vector_length(to_target);
        const float max_reach = (upper + lower) * 0.999f;
        const float min_reach = std::max(std::fabs(upper - lower) * 1.001f, 1e-4f);
        if (reach < 1e-6f) continue;
        const Vector3 target_dir = to_target / reach;
        reach = std::clamp(reach, min_reach, max_reach);
        const Vector3 target = a + target_dir * reach;

        // Bend plane. The pole, when there is one, decides which way the knee
        // points; without one the plane the animation already had is kept, which is
        // almost always right and never flips.
        Vector3 bend_axis;
        if (chain.has_pole) {
            const Vector3 to_pole = chain.pole - a;
            bend_axis = Vector3::cross(target_dir, to_pole);
        } else {
            bend_axis = Vector3::cross(b - a, c - a);
        }
        if (vector_length(bend_axis) < 1e-6f) {
            // Colinear limb and no usable pole: any axis perpendicular to the limb
            // will do, and this one is stable.
            const Vector3 fallback = std::fabs(target_dir.y) < 0.9f ? Vector3{ 0.0f, 1.0f, 0.0f }
                                                                   : Vector3{ 1.0f, 0.0f, 0.0f };
            bend_axis = Vector3::cross(target_dir, fallback);
        }
        bend_axis = bend_axis.normalized();

        // Law of cosines for both interior angles at the solved pose.
        const float cos_root = std::clamp(
            (upper * upper + reach * reach - lower * lower) / (2.0f * upper * reach), -1.0f, 1.0f);
        const float cos_mid = std::clamp(
            (upper * upper + lower * lower - reach * reach) / (2.0f * upper * lower), -1.0f, 1.0f);
        const float want_root = std::acos(cos_root);
        const float want_mid = std::acos(cos_mid);

        // ...and for the pose as animated, so the solve is expressed as a delta.
        const float current_reach = vector_length(c - a);
        const float cur_cos_root = std::clamp(
            (upper * upper + current_reach * current_reach - lower * lower) /
                (2.0f * upper * std::max(current_reach, 1e-6f)), -1.0f, 1.0f);
        const float cur_cos_mid = std::clamp(
            (upper * upper + lower * lower - current_reach * current_reach) /
                (2.0f * upper * lower), -1.0f, 1.0f);
        const float have_root = std::acos(cur_cos_root);
        const float have_mid = std::acos(cur_cos_mid);

        // Weight scales the angles, so a partially weighted chain lands part of the
        // way to the solve rather than snapping and then blending.
        const float w = chain.weight;

        // 1. Bend the knee/elbow.
        const Vector4 mid_rotation = quat_from_axis_angle(bend_axis, (want_mid - have_mid) * w);
        mid_mesh = rotate_about(mid_mesh, b, mid_rotation);
        end_mesh = rotate_about(end_mesh, b, mid_rotation);

        // 2. Open the root by its share of the same triangle.
        const Vector4 root_bend = quat_from_axis_angle(bend_axis, (want_root - have_root) * w);
        root_mesh = rotate_about(root_mesh, a, root_bend);
        mid_mesh = rotate_about(mid_mesh, a, root_bend);
        end_mesh = rotate_about(end_mesh, a, root_bend);

        // 3. Swing the whole limb so the tip lands on the target. After the two
        //    bends the tip is at the right distance, so this is pure rotation.
        const Vector3 tip = matrix_position(end_mesh);
        const Vector3 have_dir = (tip - a).normalized();
        const Vector3 want_dir = (target - a).normalized();
        Vector4 swing = rotation_between(have_dir, want_dir);
        if (w < 1.0f) {
            swing = Vector4::quat_slerp({ 0.0f, 0.0f, 0.0f, 1.0f }, swing, w);
        }
        root_mesh = rotate_about(root_mesh, a, swing);
        mid_mesh = rotate_about(mid_mesh, a, swing);
        end_mesh = rotate_about(end_mesh, a, swing);

        // Foot placement asks for an extra rotation on the tip, to lie the foot
        // along the surface it found rather than along the leg.
        if (chain.has_end_delta) {
            end_mesh = rotate_about(end_mesh, matrix_position(end_mesh), chain.end_delta);
        }

        global_transforms[root] = from_mesh * root_mesh;
        global_transforms[mid] = from_mesh * mid_mesh;
        global_transforms[end] = from_mesh * end_mesh;
        ik_dirty[root] = ik_dirty[mid] = ik_dirty[end] = 1;
        lowest_dirty = std::min(lowest_dirty, root);
    }

    if (lowest_dirty < bone_count) refresh_descendants(lowest_dirty);
}

// ===========================================================================
//  Foot placement
// ===========================================================================

void AnimationPlayer::set_ground_probe(GroundProbe probe) {
    ground_probe = std::move(probe);
}

void AnimationPlayer::set_world_transform(const Matrix4x4& transform) {
    mesh_to_world = transform;
    world_to_mesh = transform.inverse();
}

int AnimationPlayer::configure_foot_placement(const std::string& pelvis_bone_name,
                                              const std::vector<std::string>& foot_bone_names) {
    if (!skeleton) return 0;
    foot_chains.clear();
    pelvis_bone = skeleton->find_bone(pelvis_bone_name);

    int configured = 0;
    for (const std::string& foot_name : foot_bone_names) {
        const int foot = skeleton->find_bone(foot_name);
        if (foot < 0) continue;
        // add_two_bone_ik rejects a tip without a shin and a thigh above it, which
        // is exactly the condition for a foot this cannot drive.
        const int chain = add_two_bone_ik(foot_name);
        if (chain < 0) continue;

        FootChain fc;
        fc.foot_bone = foot;
        fc.chain = chain;
        foot_chains.push_back(fc);
        ++configured;
    }
    return configured;
}

void AnimationPlayer::update_foot_placement(float delta_seconds) {
    if (!foot_settings.enabled || !ground_probe || foot_chains.empty() || !skeleton) {
        // Release the chains so a disabled solver leaves the animation alone rather
        // than holding the last targets it computed.
        for (const FootChain& foot : foot_chains) {
            if (foot.chain >= 0 && foot.chain < static_cast<int>(ik_chains.size())) {
                ik_chains[foot.chain].enabled = false;
                ik_chains[foot.chain].has_end_delta = false;
            }
        }
        pelvis_offset = 0.0f;
        return;
    }

    // Exponential smoothing expressed as a half-life, so the response is the same
    // regardless of frame rate. At zero it is instant, which pops on stair edges.
    const float half_life = std::max(foot_settings.adjust_half_life, 0.0f);
    const float blend = (half_life <= 1e-5f)
        ? 1.0f
        : 1.0f - std::exp2(-delta_seconds / half_life);

    const Matrix4x4& to_mesh = skeleton->global_inverse_transform;

    // Pass one: trace under every foot and record how far each needs to move.
    float lowest_required = 0.0f;
    for (FootChain& foot : foot_chains) {
        foot.had_ground = false;
        if (foot.foot_bone < 0 || foot.foot_bone >= static_cast<int>(global_transforms.size())) {
            continue;
        }

        const Matrix4x4 foot_mesh = to_mesh * global_transforms[foot.foot_bone];
        const Vector3 foot_local = matrix_position(foot_mesh);
        const Vector3 foot_world = mesh_to_world * foot_local;

        const DVector3 from{ static_cast<double>(foot_world.x),
                             static_cast<double>(foot_world.y) + foot_settings.trace_up,
                             static_cast<double>(foot_world.z) };
        const float distance = foot_settings.trace_up + foot_settings.trace_down;

        Vector3 hit_point, hit_normal;
        if (!ground_probe(from, Vector3{ 0.0f, -1.0f, 0.0f }, distance, hit_point, hit_normal)) {
            // No ground: let this foot fall back to the animated pose rather than
            // holding the offset it had over the last surface.
            foot.offset += (0.0f - foot.offset) * blend;
            continue;
        }

        foot.had_ground = true;
        foot.ground_normal = hit_normal;
        const float desired_y = hit_point.y + foot_settings.foot_height;
        const float required = desired_y - foot_world.y;
        foot.target_offset = required;
        lowest_required = std::min(lowest_required, required);
    }

    // Pass two: the pelvis drops by the deepest correction any foot needed, so no
    // leg has to over-extend to reach its ground. It only ever drops - raising it
    // would lift the character off the surface the other foot is standing on.
    const float wanted_pelvis = std::max(lowest_required, -foot_settings.max_pelvis_drop);
    pelvis_offset += (std::min(wanted_pelvis, 0.0f) - pelvis_offset) * blend;

    if (pelvis_bone >= 0 && pelvis_bone < static_cast<int>(global_transforms.size()) &&
        std::fabs(pelvis_offset) > 1e-6f) {
        // The drop is a world-space vertical, so it is applied in mesh space along
        // whatever direction world-up maps to.
        const Vector3 world_up_in_mesh = transform_direction(world_to_mesh, { 0.0f, 1.0f, 0.0f });
        const Vector3 shift = world_up_in_mesh * pelvis_offset;
        const Matrix4x4 from_mesh = to_mesh.inverse();
        Matrix4x4 pelvis_mesh = to_mesh * global_transforms[pelvis_bone];
        pelvis_mesh = Matrix4x4::translation(shift) * pelvis_mesh;
        global_transforms[pelvis_bone] = from_mesh * pelvis_mesh;
        // Everything below the pelvis has to follow it before the feet are solved,
        // or each leg is solved from a hip that has not moved yet.
        reproject_from(pelvis_bone);
    }

    // Pass three: aim each foot's chain at its corrected position.
    const float max_align = foot_settings.max_align_degrees * 3.14159265f / 180.0f;
    for (FootChain& foot : foot_chains) {
        if (foot.chain < 0 || foot.chain >= static_cast<int>(ik_chains.size())) continue;
        IKChain& chain = ik_chains[foot.chain];

        if (!foot.had_ground) {
            chain.enabled = false;
            chain.has_end_delta = false;
            continue;
        }

        // The pelvis has already moved, so the offset this foot still needs is
        // measured against where it is now, not where it was before the drop.
        const Matrix4x4 foot_mesh = to_mesh * global_transforms[foot.foot_bone];
        const Vector3 foot_local = matrix_position(foot_mesh);

        foot.offset += (foot.target_offset - foot.offset) * blend;

        const Vector3 world_up_in_mesh = transform_direction(world_to_mesh, { 0.0f, 1.0f, 0.0f });
        chain.target = foot_local + world_up_in_mesh * foot.offset;
        chain.has_target = true;
        chain.enabled = true;
        chain.weight = 1.0f;

        // Tilt the sole onto the surface, capped so a foot cannot end up lying flat
        // against a wall the trace happened to catch.
        if (foot_settings.align_to_normal) {
            const Vector3 normal_mesh = transform_direction(world_to_mesh, foot.ground_normal).normalized();
            const Vector3 up_mesh = world_up_in_mesh.normalized();
            const float angle = std::acos(std::clamp(Vector3::dot(up_mesh, normal_mesh), -1.0f, 1.0f));
            if (angle > 1e-4f && angle <= max_align) {
                chain.end_delta = rotation_between(up_mesh, normal_mesh);
                chain.has_end_delta = true;
            } else {
                chain.has_end_delta = false;
            }
        } else {
            chain.has_end_delta = false;
        }
    }
}

// Rebuilds every bone below `bone` from the (already updated) parent chain.
void AnimationPlayer::reproject_from(int bone) {
    if (!skeleton) return;
    const int bone_count = skeleton->bone_count();
    std::vector<Matrix4x4> local(bone_count);
    for (int i = 0; i < bone_count; ++i) {
        const int parent = skeleton->bones[i].parent_index;
        local[i] = (parent >= 0 && parent < i)
            ? global_transforms[parent].inverse() * global_transforms[i]
            : global_transforms[i];
    }
    for (int i = bone + 1; i < bone_count; ++i) {
        const int parent = skeleton->bones[i].parent_index;
        if (parent >= 0 && parent < i) {
            global_transforms[i] = global_transforms[parent] * local[i];
        }
    }
}
