#pragma once

#include "core/math.hpp"
#include <string>
#include <vector>

class AnimationPlayer;

// An authorable animation state machine, sitting on top of AnimationPlayer's
// blending primitives.
//
// The machine never evaluates a pose itself. Every frame it produces one number per
// clip - a weight vector that sums to 1 - and pushes those weights and cursors into
// the player, which then does the blending it already knew how to do. That split is
// what keeps a blend space, a crossfade and a hand-driven additive overlay all
// going through exactly one pose evaluator instead of three that disagree.
//
// Evaluation is deterministic: no hash containers, no pointer ordering, no wall
// clock. Given the same parameter values and the same sequence of delta times, the
// machine visits the same states and emits the same weights, which is the only way
// a transition bug is debuggable at all.

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

// The interface between gameplay and animation. Script (or C++) sets named values;
// transitions read them. Nothing else crosses the boundary, so a designer can
// rewire the graph without touching code and a programmer can change how Speed is
// computed without touching the graph.
enum class AnimParamType {
    Float = 0,
    Int = 1,
    Bool = 2,
    // A one-shot: set by gameplay, consumed by the first transition that fires on
    // it. Modelled separately from Bool because "jump now" and "is jumping" behave
    // differently - a bool the caller forgets to clear latches a state forever.
    Trigger = 3,
};

class AnimParameterSet {
public:
    // Adding an existing name returns the existing index and leaves its value
    // alone, so reloading a graph does not reset what gameplay already wrote.
    int add_float(const std::string& name, float value = 0.0f);
    int add_int(const std::string& name, int value = 0);
    int add_bool(const std::string& name, bool value = false);
    int add_trigger(const std::string& name);

    int find(const std::string& name) const;
    int count() const { return static_cast<int>(entries.size()); }
    const std::string& name_of(int index) const;
    AnimParamType type_of(int index) const;

    // Index forms. Out of range is a silent no-op (or zero), because a graph that
    // lost a parameter should degrade to "condition never passes", not crash.
    void set_float_at(int index, float value);
    void set_int_at(int index, int value);
    void set_bool_at(int index, bool value);
    void set_trigger_at(int index);
    void clear_trigger_at(int index);

    float get_float_at(int index) const;
    int   get_int_at(int index) const;
    bool  get_bool_at(int index) const;
    bool  get_trigger_at(int index) const;

    // Name forms, which is what gameplay uses. Return false for an unknown name so
    // a typo in a script is visible rather than silently doing nothing forever.
    bool set_float(const std::string& name, float value);
    bool set_int(const std::string& name, int value);
    bool set_bool(const std::string& name, bool value);
    bool set_trigger(const std::string& name);
    bool clear_trigger(const std::string& name);

    float get_float(const std::string& name) const;
    int   get_int(const std::string& name) const;
    bool  get_bool(const std::string& name) const;
    bool  get_trigger(const std::string& name) const;

    void clear_all_triggers();

private:
    // A vector, not a map: iteration order is part of the machine's determinism and
    // the count is small enough that a linear find is faster than hashing anyway.
    struct Entry {
        std::string name;
        AnimParamType type = AnimParamType::Float;
        float f = 0.0f;
        int   i = 0;
        bool  b = false;
    };
    std::vector<Entry> entries;
};

// ---------------------------------------------------------------------------
// Conditions and transitions
// ---------------------------------------------------------------------------

enum class AnimCompare {
    Greater = 0,
    GreaterEqual,
    Less,
    LessEqual,
    Equal,
    NotEqual,
    // Bool/Trigger forms; threshold is ignored.
    IsTrue,
    IsFalse,
};

struct AnimCondition {
    int parameter = -1;
    AnimCompare compare = AnimCompare::Greater;
    float threshold = 0.0f;
};

// How a transition's weight travels from 0 to 1. Linear is honest but reads as a
// slight snap at both ends on long blends; the eases are what make a 0.4s
// locomotion transition look hand-authored.
enum class AnimBlendCurve {
    Linear = 0,
    EaseIn,
    EaseOut,
    EaseInOut,
};

float anim_apply_blend_curve(AnimBlendCurve curve, float t);

struct AnimTransition {
    // -1 means "from any state" - the hit reaction and death case. Any-state
    // transitions are evaluated before the current state's own, so a death can
    // always cut in.
    int from = -1;
    int to = -1;
    // Every condition must hold. An OR is expressed as two transitions, which keeps
    // the evaluation order (and therefore the priority) visible in the file.
    std::vector<AnimCondition> conditions;

    float duration = 0.2f;
    AnimBlendCurve curve = AnimBlendCurve::EaseInOut;

    // Gate on the source state's normalized time, so a transition can be pinned to
    // the end of a one-shot ("only leave the attack once it has actually played").
    bool  has_exit_time = false;
    float exit_time = 1.0f;

    // Whether this transition, once running, may itself be cut short. A false here
    // is what makes a dodge roll commit: nothing else fires until the blend into it
    // has finished. The current pose is carried into the interrupting transition,
    // so cutting one short never snaps.
    bool interruptible = true;

    // Any-state only. Without it an any-state transition would re-fire into the
    // state that is already current every frame its conditions hold, restarting the
    // clip forever.
    bool to_self = false;

    // Carries the source state's normalized time into the destination instead of
    // restarting it. This is what keeps the feet in step across a walk -> run
    // transition; leave it off for anything that must start at frame one.
    bool sync_phase = false;
};

// ---------------------------------------------------------------------------
// States and blend spaces
// ---------------------------------------------------------------------------

enum class AnimNodeKind {
    Clip = 0,
    // speed -> idle/walk/run. Weights come from the bracketing pair of samples.
    BlendSpace1D,
    // direction x speed -> a strafe set. Weights come from gradient bands.
    BlendSpace2D,
};

struct AnimBlendSample {
    Vector2 position = { 0.0f, 0.0f };
    int clip = -1;
    // Per-sample rate correction, for a run cycle authored at the wrong tempo.
    // Divides into the sample's duration when the state's blended period is
    // computed, so raising it makes that sample contribute a shorter cycle.
    float speed = 1.0f;
};

struct AnimGraphState {
    std::string name;
    AnimNodeKind kind = AnimNodeKind::Clip;
    // Clip states carry exactly one sample. Blend spaces carry as many as were
    // authored; a blend space with one sample degenerates to a clip state.
    std::vector<AnimBlendSample> samples;
    int param_x = -1;
    int param_y = -1;
    bool loop = true;
    float speed = 1.0f;
    // Multiplies `speed` by a float parameter, which is how a locomotion state is
    // made to keep its feet on the ground as the character accelerates.
    int speed_parameter = -1;
};

// ---------------------------------------------------------------------------
// The machine
// ---------------------------------------------------------------------------

class AnimStateMachine {
public:
    // Weight below which a state or sample contributes nothing measurable.
    static constexpr float kWeightEpsilon = 1e-4f;
    // How many transitions may be in flight at once before the oldest are folded
    // together. Eight is far more than a readable graph produces; the cap exists so
    // a graph that thrashes cannot grow this list without bound.
    static constexpr int kMaxConcurrentFades = 8;

    AnimParameterSet& parameters() { return params; }
    const AnimParameterSet& parameters() const { return params; }

    // --- Authoring -----------------------------------------------------------
    int add_clip_state(const std::string& name, int clip_index, bool loop = true, float speed = 1.0f);
    // The parameter names must already exist; a blend space over a parameter that
    // was never declared would silently read 0 forever.
    int add_blend_space_1d(const std::string& name, const std::string& param_x);
    int add_blend_space_2d(const std::string& name, const std::string& param_x, const std::string& param_y);

    bool add_sample(int state, float x, int clip_index, float speed = 1.0f);
    bool add_sample_2d(int state, float x, float y, int clip_index, float speed = 1.0f);

    bool set_state_loop(int state, bool loop);
    bool set_state_speed(int state, float speed);
    bool set_state_speed_parameter(int state, const std::string& param);

    int state_count() const { return static_cast<int>(states.size()); }
    int find_state(const std::string& name) const;
    const std::string& state_name(int state) const;
    const AnimGraphState* get_state(int state) const;

    int add_transition(int from, int to, float duration = 0.2f);
    int add_any_transition(int to, float duration = 0.2f);
    bool add_condition(int transition, const std::string& param, AnimCompare compare, float threshold = 0.0f);
    bool set_transition_curve(int transition, AnimBlendCurve curve);
    // A negative value clears the gate.
    bool set_transition_exit_time(int transition, float normalized_time);
    bool set_transition_interruptible(int transition, bool value);
    bool set_transition_to_self(int transition, bool value);
    bool set_transition_sync_phase(int transition, bool value);
    int transition_count() const { return static_cast<int>(transitions.size()); }
    const AnimTransition* get_transition(int transition) const;

    void set_entry_state(int state) { entry_state = state; }
    int  get_entry_state() const { return entry_state; }

    // --- Runtime -------------------------------------------------------------
    // Snaps to `state` (or the entry state when negative) with no blend, discarding
    // every fade in progress. This is a teleport, not a transition.
    void reset(int state = -1);

    // One step. Advances phases, fires at most one transition, and writes the
    // resulting weights and cursors into `player`.
    //
    // Clips the machine drives are taken out of self-advancing playback: the
    // machine owns their cursors, so the player must not also advance them.
    void update(float delta_seconds, AnimationPlayer& player);

    int   get_current_state() const { return current_state; }
    bool  is_transitioning() const { return !fades.empty(); }
    float get_state_phase(int state) const;
    bool  is_state_finished(int state) const;
    // Effective weight of one state this frame, blend included. Sums to 1 over all
    // states whenever the machine has a current state.
    float get_state_weight(int state) const;
    // Per-clip weights from the most recent update. Sums to 1 (up to float error)
    // whenever the machine is running, everywhere in the parameter domain -
    // including outside the convex hull of a blend space's samples.
    const std::vector<float>& get_clip_weights() const { return clip_weights; }

    // --- Weight helpers, exposed for tests and for the editor ---------------
    // Sample weights for a blend-space state at an arbitrary parameter point,
    // without touching the machine's runtime state.
    bool evaluate_state_samples(int state, float x, float y, std::vector<float>& out_weights) const;

    // --- Text format ---------------------------------------------------------
    // Parses the line-oriented .animgraph format. `player` resolves clip names to
    // indices, so the graph is authored against clip names and survives a re-import
    // that reorders them. Returns false and fills out_error on the first problem;
    // whatever parsed before that is left in place, because a half-built graph that
    // still idles is better than a character stuck in bind pose.
    bool load_from_string(const std::string& text, const AnimationPlayer& player,
                          std::string* out_error = nullptr);
    bool load_from_file(const std::string& path, const AnimationPlayer& player,
                        std::string* out_error = nullptr);
    void clear();

private:
    struct StateRuntime {
        float phase = 0.0f;    // normalized, [0, 1)
        bool finished = false; // a non-looping state has reached its end
    };

    // One transition still blending out. Weights are defined so that the sum over
    // every fade plus the current state is exactly 1 at all times: a new fade
    // starts at whatever weight the current state held, so the moment it is pushed
    // the fades total 1 and the incoming state starts at 0.
    struct Fade {
        int state = -1;
        float weight_at_start = 0.0f;
        float elapsed = 0.0f;
        float duration = 0.0f;
        AnimBlendCurve curve = AnimBlendCurve::Linear;
    };

    bool valid_state(int index) const { return index >= 0 && index < static_cast<int>(states.size()); }
    bool valid_transition(int index) const { return index >= 0 && index < static_cast<int>(transitions.size()); }

    float state_period(int state, const AnimationPlayer& player) const;
    void advance_state(int state, float delta_seconds, const AnimationPlayer& player);
    bool conditions_pass(const AnimTransition& transition) const;
    int  pick_transition() const;
    void fire_transition(int transition_index);
    float fade_weight(const Fade& fade) const;
    void accumulate_state(int state, float weight, const AnimationPlayer& player);

    AnimParameterSet params;
    std::vector<AnimGraphState> states;
    std::vector<AnimTransition> transitions;
    int entry_state = -1;

    std::vector<StateRuntime> runtime;
    std::vector<Fade> fades;
    int current_state = -1;
    // Set while a non-interruptible transition is blending. Nothing fires until it
    // finishes, which is what "commit to this move" means.
    bool blocked = false;

    std::vector<float> clip_weights;
    // Which contributor set each clip's cursor, so the heaviest one wins. A clip
    // has exactly one playback cursor in the player, so when two states both use it
    // only one of them can decide where it is.
    std::vector<float> clip_time_authority;
    std::vector<float> clip_times;
    // Scratch, so evaluating a blend space allocates nothing per frame.
    mutable std::vector<float> sample_weights;
};
