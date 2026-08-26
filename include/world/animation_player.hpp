#pragma once

#include "world/skeleton.hpp"
#include "world/animation_clip.hpp"
#include <string>
#include <vector>

// Drives one skinned mesh's pose.
//
// The skeleton and the clips belong to the MeshResource (they are per-asset and
// shared between every actor using that mesh); only the playback cursor and the
// resulting palette live here, so two actors can play different clips off the same
// imported model. The owner is responsible for keeping that resource alive for as
// long as the player exists.
//
// Several clips can be active at once. Each has its own weight, layer and blend
// mode, and the final pose is their weighted blend - which is what makes a walk
// blend smoothly into a run instead of snapping, and what lets an aim offset play
// on the upper body while the legs keep walking. A player with exactly one state at
// weight 1 behaves identically to the single-clip playback this replaced.
class AnimationPlayer {
public:
    // How a state combines with what lower layers already produced.
    //
    // Blend replaces: the state's pose is interpolated over the accumulated one by
    // its weight, which is what a walk/run/idle transition wants.
    //
    // Additive applies the clip as a *difference* from its own first frame, so a
    // lean, a limp or an aim offset can be layered on top of whatever the character
    // is already doing without erasing it.
    enum class BlendMode { Blend = 0, Additive = 1 };

    // Weight below which a state contributes nothing measurable and is skipped.
    // Also the threshold at which a fade-out is considered finished.
    static constexpr float kWeightEpsilon = 1e-4f;

    AnimationPlayer(const Skeleton* skeleton, const std::vector<AnimationClip>* clips);

    // Advances every active state's cursor, steps any fade in progress, and
    // re-evaluates the blended pose. Safe to call with nothing playing - it still
    // produces a valid palette (the rest pose).
    void update(float delta_seconds);

    // --- Single-clip playback ------------------------------------------------
    // Starts one clip at full weight and stops everything else immediately. This is
    // the hard cut; crossfade() is what gameplay normally wants.
    //
    // Out-of-range indices and unknown names stop playback rather than throwing, so
    // a scene referencing a renamed clip degrades to the rest pose instead of
    // crashing.
    void play(int clip_index, bool loop = true);
    bool play(const std::string& clip_name, bool loop = true);
    // Stops every state and returns to the rest pose.
    void stop();

    // --- Blending ------------------------------------------------------------
    // Fades this clip in over fade_seconds while fading out every other Blend-mode
    // state on the same layer. A clip that is already running keeps its cursor, so
    // calling this every frame from a state machine does not restart it.
    void crossfade(int clip_index, float fade_seconds, bool loop = true);
    bool crossfade(const std::string& clip_name, float fade_seconds, bool loop = true);

    // Fades one clip toward target_weight without touching any other state. This is
    // the layering primitive: an additive lean at 0.3, a hurt overlay fading in.
    // A target of zero fades the clip out and stops it once it reaches silence.
    void blend(int clip_index, float target_weight, float fade_seconds);
    bool blend(const std::string& clip_name, float target_weight, float fade_seconds);

    // Stops one clip immediately, leaving the rest of the blend alone.
    void stop_clip(int clip_index);
    bool stop_clip(const std::string& clip_name);

    // --- Per-state configuration ---------------------------------------------
    // Layers are evaluated in ascending order and each one blends over the result of
    // those below it, so a higher layer at weight 1 fully overrides a lower one.
    // Every clip starts on layer 0.
    void  set_clip_layer(int clip_index, int layer);
    int   get_clip_layer(int clip_index) const;
    void  set_clip_blend_mode(int clip_index, BlendMode mode);
    BlendMode get_clip_blend_mode(int clip_index) const;
    // Sets the weight outright, cancelling any fade in progress.
    void  set_clip_weight(int clip_index, float weight);
    float get_clip_weight(int clip_index) const;
    void  set_clip_speed(int clip_index, float speed);
    float get_clip_speed(int clip_index) const;
    void  set_clip_time(int clip_index, float seconds);
    float get_clip_time(int clip_index) const;
    void  set_clip_looping(int clip_index, bool loop);
    bool  get_clip_looping(int clip_index) const;
    bool  is_clip_playing(int clip_index) const;

    // --- Bone masks ----------------------------------------------------------
    // Restricts a state to part of the skeleton. Masked-out bones take their pose
    // from the layers below instead, which is how an upper-body-only layer works:
    // mask a firing animation to the spine and the legs keep running underneath.
    //
    // Returns false if the bone name is not in this skeleton, leaving the mask
    // unchanged. Including descendants is almost always what is wanted - masking a
    // shoulder without its arm produces a detached limb.
    bool set_clip_bone_mask(int clip_index, const std::string& root_bone_name, bool include_descendants = true);
    // Adds another sub-tree to an existing mask, for a mask spanning both arms.
    bool add_clip_bone_mask(int clip_index, const std::string& root_bone_name, bool include_descendants = true);
    void clear_clip_bone_mask(int clip_index);
    bool has_clip_bone_mask(int clip_index) const;

    // --- Queries -------------------------------------------------------------
    int  get_clip_count() const { return clips ? static_cast<int>(clips->size()) : 0; }
    const std::string& get_clip_name(int index) const;
    int  find_clip(const std::string& clip_name) const;
    // The clip most recently started. Legacy accessors below operate on its state.
    int  get_current_clip() const { return current_clip; }
    float get_duration_seconds() const;
    float get_clip_duration_seconds(int clip_index) const;
    float get_time_seconds() const;
    void set_time_seconds(float seconds);
    // Number of states contributing to this frame's pose, for the editor to show.
    int  get_active_state_count() const;

    // True while any state is advancing. set_playing pauses or resumes all of them,
    // which is what the editor's play/pause button means.
    bool is_playing() const;
    void set_playing(bool value);
    bool is_looping() const;
    void set_looping(bool value);
    float get_speed() const;
    void set_speed(float value);

    int get_bone_count() const { return skeleton ? skeleton->bone_count() : 0; }

    // Skinning palette: one matrix per bone, mapping a bind-pose mesh-space vertex
    // to its posed position. Sized to the skeleton and never empty once constructed
    // with a valid skeleton, so the renderer can upload it unconditionally.
    const std::vector<Matrix4x4>& get_bone_matrices() const { return bone_matrices; }

private:
    // A bone's transform relative to its parent, kept as separate components rather
    // than a matrix because that is the only space blending is meaningful in.
    struct BonePose {
        Vector3 position = { 0.0f, 0.0f, 0.0f };
        Vector4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
        Vector3 scale    = { 1.0f, 1.0f, 1.0f };
    };

    // One clip's playback state. There is exactly one per clip in the asset, so a
    // clip cannot be playing twice at once - the same model the editor's clip list
    // and the scripting API present.
    struct AnimationState {
        float time = 0.0f;
        float speed = 1.0f;
        float weight = 0.0f;
        bool  looping = true;
        bool  playing = false;
        int   layer = 0;
        BlendMode mode = BlendMode::Blend;

        // Linear fade in progress. Weight moves from fade_from to fade_to over
        // fade_duration; a zero duration is applied instantly.
        bool  fading = false;
        float fade_from = 0.0f;
        float fade_to = 0.0f;
        float fade_duration = 0.0f;
        float fade_elapsed = 0.0f;
        // Set when the fade's destination is silence, so the state can stop itself
        // rather than idling at weight 0 and being sampled for nothing.
        bool  stop_when_faded = false;

        // Per-bone multiplier, or empty to mean "every bone at 1". Empty rather than
        // a vector of ones so the common unmasked case costs no memory and no
        // per-bone multiply.
        std::vector<float> bone_mask;

        // Resolved lazily the first time the state is started: binding every clip up
        // front would walk the channel list of clips that are never played.
        std::vector<const BoneChannel*> channel_for_bone;
        bool channels_bound = false;
    };

    // A state that will actually contribute to this frame, with its per-frame
    // constants resolved once instead of per bone.
    struct PreparedState {
        const AnimationState* state = nullptr;
        const AnimationClip* clip = nullptr;
        float ticks = 0.0f;
        // Additive only: the tick the clip's reference (first) frame sits at.
        float reference_ticks = 0.0f;
    };

    bool valid_clip(int clip_index) const {
        return clips && clip_index >= 0 && clip_index < static_cast<int>(clips->size());
    }
    AnimationState* state_for(int clip_index);
    const AnimationState* state_for(int clip_index) const;

    void bind_channels(int clip_index);
    // Starts a state if it is not already running, without disturbing its weight.
    // Returns the state, or null for an invalid clip index.
    AnimationState* begin_state(int clip_index, bool loop);
    void start_fade(AnimationState& state, float target_weight, float fade_seconds, bool stop_at_zero);
    void advance_state(AnimationState& state, const AnimationClip& clip, float delta_seconds);

    void evaluate_pose();
    // Samples one state's local pose for one bone, falling back to the rest pose for
    // bones the clip does not animate.
    BonePose sample_state(const PreparedState& prepared, int bone_index, float ticks) const;
    float state_bone_weight(const AnimationState& state, int bone_index) const;
    bool apply_bone_mask(int clip_index, const std::string& root_bone_name, bool include_descendants, bool reset_existing);

    const Skeleton* skeleton = nullptr;
    const std::vector<AnimationClip>* clips = nullptr;

    std::vector<AnimationState> states; // one per clip, parallel to *clips

    int   current_clip = -1;
    // Applied to states created from here on, so set_speed() before the first
    // play() is not silently dropped.
    float default_speed = 1.0f;
    bool  default_looping = true;
    // Set whenever a cursor, weight or fade changes, so a fully paused player does
    // not re-blend an identical pose every frame.
    bool  pose_dirty = true;

    // The imported rest pose, decomposed once. Bones no active clip animates hold
    // this, and it is also the base every layer blends over.
    std::vector<BonePose> rest_pose;

    std::vector<PreparedState> prepared;      // scratch, rebuilt per evaluation
    std::vector<int> layer_order;             // scratch, distinct layers ascending
    std::vector<BonePose> accumulated;        // scratch, indexed by bone
    // Which bones any active state actually wrote this frame. A bone nothing touched
    // takes its matrix straight from the skeleton rather than being recomposed from
    // the decomposed rest pose, so a partial or masked clip leaves the rest of the
    // rig bit-for-bit as the importer produced it.
    std::vector<unsigned char> bone_written;
    std::vector<Matrix4x4> global_transforms; // scratch, indexed by bone
    std::vector<Matrix4x4> bone_matrices;
};
