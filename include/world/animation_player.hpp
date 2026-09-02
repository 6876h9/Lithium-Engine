#pragma once

#include "world/skeleton.hpp"
#include "world/animation_clip.hpp"
#include "world/anim_graph.hpp"
#include <functional>
#include <memory>
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

    // What an additive state measures its difference against.
    //
    // FirstFrame is the authoring convention for a clip exported as a normal
    // animation whose frame zero is the neutral pose - a recoil, a limp, a breath.
    // RestPose is what an aim offset wants: the offset poses are authored straight
    // against the bind pose, and there is no neutral frame inside the clip to
    // subtract. Getting this wrong does not error, it just doubles or cancels the
    // offset, so it is worth naming explicitly per clip.
    enum class AdditiveReference { FirstFrame = 0, RestPose = 1 };

    // Weight below which a state contributes nothing measurable and is skipped.
    // Also the threshold at which a fade-out is considered finished.
    static constexpr float kWeightEpsilon = 1e-4f;

    AnimationPlayer(const Skeleton* skeleton, const std::vector<AnimationClip>* clips);
    ~AnimationPlayer();

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

    // --- Additive layers -----------------------------------------------------
    // An additive state is applied as a difference from its reference pose, after
    // every Blend-mode state on the same layer has been resolved. The recipe for an
    // aim offset is: put the offset clip on a layer above the locomotion base, set
    // it Additive with a RestPose reference, mask it to the spine, and drive its
    // weight from how far off-centre the aim is. For recoil or breathing, use
    // FirstFrame instead and leave the mask off.
    //
    // Additive states are never crossfaded out by crossfade(): a lean is not an
    // alternative to the walk cycle, so changing the base animation underneath an
    // overlay leaves the overlay alone.
    void set_clip_additive_reference(int clip_index, AdditiveReference reference,
                                     float reference_seconds = 0.0f);
    AdditiveReference get_clip_additive_reference(int clip_index) const;
    float get_clip_additive_reference_time(int clip_index) const;

    // --- Root motion ---------------------------------------------------------
    // The root bone's per-frame delta, taken out of the pose and handed to whoever
    // is actually moving the character.
    //
    // ORDERING: Engine::update() advances animation for every mesh *before* it
    // steps joints, physics, nav agents and character controllers. That is what
    // makes this safe to consume from the character step in the same frame; if
    // animation were moved after it, the character would lag the animation by a
    // frame and foot-sliding would appear on every clip. Do not reorder those.
    struct RootMotion {
        // Mesh-space translation produced since the last update.
        Vector3 translation = { 0.0f, 0.0f, 0.0f };
        // Mesh-space rotation delta produced since the last update.
        Vector4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
        // True when any state with root motion enabled contributed this frame.
        bool valid = false;
    };

    // Defaults to the first bone with no parent. Only worth setting when a rig
    // carries a separate motion bone above the hips.
    bool set_root_bone(const std::string& bone_name);
    int  get_root_bone() const { return root_bone; }

    // Per clip, because a rig normally mixes the two: an attack that lunges carries
    // motion, the idle it returns to does not. A clip with this off is played in
    // place exactly as before.
    void set_clip_root_motion(int clip_index, bool enabled,
                              bool translation = true, bool rotation = true);
    bool get_clip_root_motion(int clip_index) const;

    // Motion produced by the most recent update(). Re-reading it does not clear it.
    const RootMotion& get_root_motion() const { return root_motion; }
    // Returns the accumulated motion and zeroes it, so exactly one consumer moves
    // the character. Reading without consuming would let a second consumer apply
    // the same displacement twice.
    RootMotion consume_root_motion();
    // Yaw component of the root motion rotation, in radians - which is all a
    // character controller standing on the ground can use.
    static float root_motion_yaw(const RootMotion& motion);

    // --- Inverse kinematics --------------------------------------------------
    // Analytic two-bone solver, in mesh space (the same space the vertices are in).
    // `end_bone_name` is the tip - the hand or the foot; the bones actually rotated
    // are its parent and its grandparent, so the chain is grandparent -> parent ->
    // tip and nothing else in the rig moves except what hangs off them.
    //
    // Out of range is not an error: the target is clamped onto the sphere of
    // maximum reach, so the limb straightens toward an unreachable target instead
    // of tearing or flipping. A target inside the minimum reach is clamped the same
    // way. Every solve is a rotation of two joints, so bone lengths never change.
    int  add_two_bone_ik(const std::string& end_bone_name);
    int  get_ik_count() const { return static_cast<int>(ik_chains.size()); }
    bool set_ik_target(int handle, const Vector3& mesh_space_target);
    // The knee/elbow is placed on the side of the limb axis that this point lies
    // on. Without one the solver keeps the bend direction the animation already
    // had, which is usually right and never flips.
    bool set_ik_pole(int handle, const Vector3& mesh_space_pole);
    bool clear_ik_pole(int handle);
    bool set_ik_weight(int handle, float weight);
    float get_ik_weight(int handle) const;
    bool set_ik_enabled(int handle, bool enabled);
    bool is_ik_enabled(int handle) const;
    // Bone indices resolved for a chain, for the editor and for tests.
    bool get_ik_bones(int handle, int& out_root, int& out_mid, int& out_end) const;

    // Where a bone ended up, in mesh space, after the most recent evaluation.
    // This is the only way to check an IK solve actually reached its target.
    bool get_bone_mesh_position(int bone_index, Vector3& out_position) const;
    bool get_bone_mesh_transform(int bone_index, Matrix4x4& out_transform) const;

    // --- Foot placement ------------------------------------------------------
    // Traces down from each foot, drops the pelvis so no leg has to over-extend,
    // and pulls each foot up onto the surface under it. This is what stops a
    // character floating over a slope or clipping through a stair tread.
    //
    // THREADING: the probe is called from update(), which the engine drives from a
    // sequential pass on the logic thread - not from actor tick(), which runs
    // across the task graph. Jolt's query scratch allocator is a non-thread-safe
    // linear allocator, so a probe that raycasts must stay on that pass.
    //
    // The probe is a callback rather than a direct physics call so this file has no
    // dependency on the physics engine, and so a test can supply a flat ground
    // plane without a physics world at all.
    using GroundProbe = std::function<bool(const DVector3& world_from,
                                           const Vector3& world_direction,
                                           float max_distance,
                                           Vector3& out_point,
                                           Vector3& out_normal)>;
    void set_ground_probe(GroundProbe probe);
    bool has_ground_probe() const { return static_cast<bool>(ground_probe); }
    // Mesh -> world, so a foot position can be traced against the physics world and
    // the result brought back. Set every frame by whoever owns the pose.
    void set_world_transform(const Matrix4x4& mesh_to_world);

    struct FootPlacementSettings {
        bool  enabled = false;
        // Trace starts this far above the animated foot and runs this far below it.
        // Up has to clear a stair riser; down decides how far a foot will reach for
        // ground that has fallen away.
        float trace_up = 0.5f;
        float trace_down = 0.8f;
        // Distance from the foot bone's origin to the sole. Zero sinks the bone
        // origin into the floor, which for most rigs is an ankle.
        float foot_height = 0.02f;
        // The pelvis never rises, only drops, and never further than this. An
        // unbounded drop turns a bad trace into a character sitting on the floor.
        float max_pelvis_drop = 0.5f;
        // Exponential smoothing on both the pelvis drop and each foot offset.
        // Zero is instant and pops on every stair edge.
        float adjust_half_life = 0.08f;
        // Tilts the foot onto the surface. The limit stops a foot lying flat on a
        // wall the trace happened to catch.
        bool  align_to_normal = true;
        float max_align_degrees = 45.0f;
    };

    // Resolves the pelvis and the feet and creates a two-bone chain per foot. Each
    // foot bone needs a parent and a grandparent (shin and thigh) or it is skipped.
    // Returns the number of feet actually set up.
    int configure_foot_placement(const std::string& pelvis_bone_name,
                                 const std::vector<std::string>& foot_bone_names);
    FootPlacementSettings& foot_placement() { return foot_settings; }
    const FootPlacementSettings& foot_placement() const { return foot_settings; }
    // Current smoothed pelvis drop in world units, negative or zero. For the editor
    // and for tests.
    float get_pelvis_offset() const { return pelvis_offset; }

    // --- State machine -------------------------------------------------------
    // Optional. When one is attached, update() steps it first and the machine owns
    // the weight and cursor of every clip it references; clips it does not
    // reference are still free for a hand-driven additive overlay.
    AnimStateMachine& ensure_state_machine();
    AnimStateMachine* get_state_machine() { return graph.get(); }
    const AnimStateMachine* get_state_machine() const { return graph.get(); }
    void clear_state_machine();

    // Drives one clip's contribution directly: sets its weight and cursor and takes
    // it out of self-advancing playback, so exactly one thing owns its timing.
    // This is how the state machine talks to the player.
    void set_clip_pose(int clip_index, float weight, float time_seconds);

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

        // Where an Additive state measures its difference from.
        AdditiveReference additive_reference = AdditiveReference::FirstFrame;
        float additive_reference_seconds = 0.0f;

        // Root motion. Off by default: an in-place clip whose root drifts a little
        // would otherwise start pushing the character around.
        bool root_motion = false;
        bool root_motion_translation = true;
        bool root_motion_rotation = true;
        // Cursor at the start of the frame, so the root delta is a difference over
        // the frame rather than an absolute that has to be remembered elsewhere.
        float previous_time = 0.0f;
        bool  wrapped_this_frame = false;

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
    // The pose an Additive state subtracts. Either a frame of the clip itself or the
    // rest pose, depending on how the clip was authored.
    BonePose sample_additive_reference(const PreparedState& prepared, int bone_index) const;
    float state_bone_weight(const AnimationState& state, int bone_index) const;
    bool apply_bone_mask(int clip_index, const std::string& root_bone_name, bool include_descendants, bool reset_existing);

    // Root motion, accumulated across every contributing state at the top of a
    // pose evaluation. Runs before the pose is composed because the root bone's
    // sampled pose has to be pinned in the same pass that reads its delta.
    void accumulate_root_motion();

    // Rebuilds mesh-space globals for `bone` and everything under it from the
    // stored local transforms. Bones are parents-first, so one forward sweep does
    // the whole sub-tree.
    void propagate_subtree(int bone);
    void solve_ik();
    void solve_two_bone(int chain_index);
    void solve_foot_placement();

    // One analytic two-bone chain. Bone indices are resolved once at creation; the
    // rig cannot change underneath a player, since a different skeleton means a
    // different player.
    struct IKChain {
        int end_bone = -1;
        int mid_bone = -1;
        int root_bone_index = -1;
        Vector3 target = { 0.0f, 0.0f, 0.0f };
        Vector3 pole = { 0.0f, 0.0f, 0.0f };
        bool has_pole = false;
        bool has_target = false;
        float weight = 1.0f;
        bool enabled = false;
        // Set by foot placement each frame, applied as a delta rotation on the end
        // bone after the chain is solved.
        bool has_end_delta = false;
        Vector4 end_delta = { 0.0f, 0.0f, 0.0f, 1.0f };
    };

    struct FootChain {
        int foot_bone = -1;
        int chain = -1;
        // Smoothed vertical correction in world units, positive upward.
        float offset = 0.0f;
        bool  had_ground = false;
        // This frame's raw trace result, before smoothing: how far the foot has to
        // move, and the surface it found.
        float target_offset = 0.0f;
        Vector3 ground_normal = { 0.0f, 1.0f, 0.0f };
    };

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

    // The local transform each bone ended up with this frame, kept so IK can
    // re-propagate a sub-tree after rotating a joint without re-blending anything.
    std::vector<Matrix4x4> local_transforms;
    // Bone globals in mesh space (global_inverse_transform already applied), which
    // is the space IK targets and foot traces are expressed in. Only filled when a
    // chain is actually active: with no IK the palette is built exactly as it was
    // before, down to the last bit, rather than through a re-associated product.
    std::vector<Matrix4x4> mesh_globals;
    std::vector<unsigned char> subtree_dirty; // scratch for propagate_subtree
    bool mesh_globals_valid = false;

    // --- Root motion ---
    int root_bone = -1;
    RootMotion root_motion;
    // Delta of the most recent update(), for the foot placement smoothing. Stored
    // rather than passed down because evaluate_pose() is also reached from every
    // setter, where there is no delta to speak of.
    float last_delta_seconds = 0.0f;

    // --- IK ---
    std::vector<IKChain> ik_chains;
    // Scratch for the IK pass. Locals are cached before the solver moves anything,
    // so a bone hanging off a solved chain can be rebuilt from its parent without
    // re-running the whole FK pass; ik_dirty marks the bones the solver itself
    // wrote, which must not then be recomposed over.
    std::vector<Matrix4x4> ik_local_cache;
    std::vector<unsigned char> ik_dirty;

    // Solves every enabled chain and re-propagates the bones below them. Runs at
    // the end of evaluate_pose(), after FK has placed the animated pose.
    void apply_ik();
    void refresh_descendants(int first_dirty);
    // Rebuilds every bone below `bone` from its parent. Used when the pelvis moves
    // and the legs have to follow before they are solved.
    void reproject_from(int bone);
    // Traces under each foot, drops the pelvis and aims each leg's chain.
    void update_foot_placement(float delta_seconds);

    std::vector<FootChain> foot_chains;
    FootPlacementSettings foot_settings;
    int pelvis_bone = -1;
    float pelvis_offset = 0.0f;
    GroundProbe ground_probe;
    Matrix4x4 mesh_to_world = Matrix4x4::identity();
    Matrix4x4 world_to_mesh = Matrix4x4::identity();

    std::unique_ptr<AnimStateMachine> graph;
};
