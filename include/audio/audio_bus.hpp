#pragma once

#include "miniaudio.h"
#include <vector>

// The DSP chain every mixer bus runs.
//
// One custom miniaudio node rather than a string of built-in ones. The chain is
// wired once at startup and never rebuilt, because editing a live node graph from
// the main thread while the audio thread is walking it is exactly the kind of race
// that produces an intermittent crash nobody can reproduce. Disabled stages are
// skipped inside the callback instead, so a bus with everything off costs a memcpy.
//
// Parameters are plain floats and bools written from the main thread and read from
// the audio thread without a lock. That is safe here: each is a single aligned word,
// so a reader sees either the old value or the new one, never half of each. The only
// consequence of the race is that two parameters changed together may be picked up
// one buffer apart, which is inaudible.
class BusEffectNode {
public:
    // ma_node_base must be the first member: miniaudio casts the node pointer it is
    // given straight to this type, and the base has to sit at offset zero.
    ma_node_base base;

    // --- Filters -------------------------------------------------------------
    // Two cascaded one-pole sections each, giving 12 dB per octave. A single pole
    // is too gentle to read as "muffled through a wall", which is what a low-pass on
    // a bus is almost always for.
    bool  lowpass_enabled = false;
    float lowpass_cutoff = 1200.0f;
    bool  highpass_enabled = false;
    float highpass_cutoff = 180.0f;

    // --- Delay ---------------------------------------------------------------
    bool  delay_enabled = false;
    float delay_seconds = 0.28f;
    // How much of each echo survives into the next. Above 1 the delay never decays.
    float delay_feedback = 0.35f;
    float delay_mix = 0.35f;

    // --- Reverb --------------------------------------------------------------
    // Freeverb: eight parallel comb filters feeding four series all-pass filters.
    // It is the standard cheap room simulation and sounds like a room rather than
    // like a delay, which is the whole reason a reverb is not just an echo.
    bool  reverb_enabled = false;
    float reverb_room_size = 0.6f;
    float reverb_damping = 0.5f;
    float reverb_wet = 0.3f;

    bool initialize(ma_node_graph* graph, ma_uint32 channels, ma_uint32 sample_rate);
    void uninitialize();
    bool is_initialized() const { return initialized; }

    // Called from the audio thread.
    void process(const float* input, float* output, ma_uint32 frame_count);

private:
    // A comb filter with a one-pole damper in its feedback path. The damper is what
    // makes high frequencies decay faster than low ones, which is what real rooms
    // do and what stops the tail sounding like ringing metal.
    struct Comb {
        std::vector<float> buffer;
        size_t index = 0;
        float store = 0.0f;
        float process(float input, float feedback, float damping);
    };

    struct Allpass {
        std::vector<float> buffer;
        size_t index = 0;
        float process(float input);
    };

    static constexpr int kCombCount = 8;
    static constexpr int kAllpassCount = 4;
    static constexpr int kMaxChannels = 8;

    struct ChannelState {
        Comb combs[kCombCount];
        Allpass allpasses[kAllpassCount];
        // Cascaded one-pole state for the two filters.
        float lowpass_state[2] = { 0.0f, 0.0f };
        float highpass_state[2] = { 0.0f, 0.0f };
        std::vector<float> delay_buffer;
        size_t delay_index = 0;
    };

    ChannelState channels_state[kMaxChannels];
    ma_uint32 channel_count = 0;
    ma_uint32 sample_rate = 48000;
    bool initialized = false;
};
