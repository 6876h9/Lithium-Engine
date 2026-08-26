#include "audio/audio_bus.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Freeverb's tunings, in samples at 44.1 kHz. They are mutually prime so the combs
// never line up into an audible periodicity, which is the whole trick: eight
// arbitrary delays would ring, these eight sound like a room.
constexpr int kCombTuning[8]    = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
constexpr int kAllpassTuning[4] = { 556, 441, 341, 225 };
// Each channel past the first offsets its filter lengths by this much, which is
// what decorrelates left from right and makes the tail feel wide instead of centred.
constexpr int kStereoSpread = 23;

// Scales the reverb input down before it enters the comb bank. Eight parallel
// filters summing at unity would clip instantly.
constexpr float kFixedGain = 0.015f;

// The comb feedback that room_size 0..1 maps onto. Below about 0.7 the tail is too
// short to read as a room at all; 0.98 is the longest that still decays.
constexpr float kRoomOffset = 0.7f;
constexpr float kRoomScale = 0.28f;
// Damping above about 0.4 kills the tail before it is audible.
constexpr float kDampScale = 0.4f;

float clamp01(float v) { return (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v); }

// One-pole smoothing coefficient for a given cutoff. Clamped below Nyquist because
// a cutoff at or above it makes the exponential collapse and the filter unstable.
float one_pole_coefficient(float cutoff_hz, ma_uint32 sample_rate) {
    const float nyquist = static_cast<float>(sample_rate) * 0.5f;
    const float cutoff = std::max(10.0f, std::min(nyquist * 0.99f, cutoff_hz));
    return 1.0f - std::exp(-2.0f * 3.14159265358979323846f * cutoff / static_cast<float>(sample_rate));
}

// miniaudio hands the node straight back to us, and ma_node_base is the first
// member, so the cast is the documented way to recover the owning object.
void bus_effect_process(ma_node* node, const float** frames_in, ma_uint32* frame_count_in,
                        float** frames_out, ma_uint32* frame_count_out) {
    BusEffectNode* self = reinterpret_cast<BusEffectNode*>(node);
    const ma_uint32 frames = std::min(*frame_count_in, *frame_count_out);
    self->process(frames_in[0], frames_out[0], frames);
    *frame_count_in = frames;
    *frame_count_out = frames;
}

const ma_node_vtable kBusEffectVTable = {
    bus_effect_process,
    nullptr, // no resampling, so miniaudio does not need a frame-count hint
    1,       // one input bus
    1,       // one output bus
    0
};

} // namespace

float BusEffectNode::Comb::process(float input, float feedback, float damping) {
    if (buffer.empty()) return input;
    const float output = buffer[index];
    // The damper is a one-pole low-pass sitting inside the feedback loop.
    store = output * (1.0f - damping) + store * damping;
    buffer[index] = input + store * feedback;
    if (++index >= buffer.size()) index = 0;
    return output;
}

float BusEffectNode::Allpass::process(float input) {
    if (buffer.empty()) return input;
    const float buffered = buffer[index];
    // Fixed 0.5 feedback: an all-pass diffuser is there to smear the comb output in
    // time without colouring it, and 0.5 is the value that leaves the magnitude
    // response flat.
    const float output = -input + buffered;
    buffer[index] = input + buffered * 0.5f;
    if (++index >= buffer.size()) index = 0;
    return output;
}

bool BusEffectNode::initialize(ma_node_graph* graph, ma_uint32 channels, ma_uint32 sample_rate_in) {
    if (initialized) return true;
    if (!graph || channels == 0) return false;

    channel_count = std::min<ma_uint32>(channels, kMaxChannels);
    sample_rate = (sample_rate_in > 0) ? sample_rate_in : 48000;

    // The tunings are quoted at 44.1 kHz; at any other rate they have to be scaled
    // or the room changes size with the output device.
    const double rate_scale = static_cast<double>(sample_rate) / 44100.0;

    for (ma_uint32 channel = 0; channel < channel_count; ++channel) {
        ChannelState& state = channels_state[channel];
        const int spread = static_cast<int>(channel) * kStereoSpread;

        for (int i = 0; i < kCombCount; ++i) {
            const size_t length = std::max<size_t>(1, static_cast<size_t>((kCombTuning[i] + spread) * rate_scale));
            state.combs[i].buffer.assign(length, 0.0f);
            state.combs[i].index = 0;
            state.combs[i].store = 0.0f;
        }
        for (int i = 0; i < kAllpassCount; ++i) {
            const size_t length = std::max<size_t>(1, static_cast<size_t>((kAllpassTuning[i] + spread) * rate_scale));
            state.allpasses[i].buffer.assign(length, 0.0f);
            state.allpasses[i].index = 0;
        }

        // Two seconds of delay line. Sized once at the maximum rather than resized
        // when delay_seconds changes, because reallocating a buffer the audio thread
        // is reading is not something a parameter change is allowed to do.
        state.delay_buffer.assign(static_cast<size_t>(sample_rate) * 2, 0.0f);
        state.delay_index = 0;
    }

    ma_node_config config = ma_node_config_init();
    config.vtable = &kBusEffectVTable;
    config.pInputChannels = &channels;
    config.pOutputChannels = &channels;

    if (ma_node_init(graph, &config, nullptr, &base) != MA_SUCCESS) return false;
    initialized = true;
    return true;
}

void BusEffectNode::uninitialize() {
    if (!initialized) return;
    ma_node_uninit(&base, nullptr);
    initialized = false;
}

void BusEffectNode::process(const float* input, float* output, ma_uint32 frame_count) {
    if (channel_count == 0 || frame_count == 0) return;

    // Read every parameter once per buffer rather than per sample. It costs nothing
    // and it means a parameter changed mid-buffer cannot produce a discontinuity
    // partway through one.
    const bool do_highpass = highpass_enabled;
    const bool do_lowpass = lowpass_enabled;
    const bool do_delay = delay_enabled;
    const bool do_reverb = reverb_enabled;

    if (!do_highpass && !do_lowpass && !do_delay && !do_reverb) {
        // Nothing enabled: this is the common case for most buses, and it should
        // cost no more than the copy miniaudio would have done anyway.
        std::memcpy(output, input, static_cast<size_t>(frame_count) * channel_count * sizeof(float));
        return;
    }

    const float lowpass_a = one_pole_coefficient(lowpass_cutoff, sample_rate);
    const float highpass_a = one_pole_coefficient(highpass_cutoff, sample_rate);

    const float feedback = kRoomOffset + clamp01(reverb_room_size) * kRoomScale;
    const float damping = clamp01(reverb_damping) * kDampScale;
    const float wet = clamp01(reverb_wet);
    const float dry = 1.0f - wet;

    const size_t delay_frames = std::max<size_t>(
        1, static_cast<size_t>(std::max(0.001f, delay_seconds) * static_cast<float>(sample_rate)));
    const float delay_fb = std::max(0.0f, std::min(0.95f, delay_feedback));
    const float delay_amount = clamp01(delay_mix);

    for (ma_uint32 frame = 0; frame < frame_count; ++frame) {
        for (ma_uint32 channel = 0; channel < channel_count; ++channel) {
            ChannelState& state = channels_state[channel];
            const size_t sample_index = static_cast<size_t>(frame) * channel_count + channel;
            float sample = input[sample_index];

            if (do_highpass) {
                // A high-pass is the signal minus its own low-passed self; cascading
                // the low-pass twice gives the same 12 dB slope as the low-pass path.
                state.highpass_state[0] += highpass_a * (sample - state.highpass_state[0]);
                state.highpass_state[1] += highpass_a * (state.highpass_state[0] - state.highpass_state[1]);
                sample -= state.highpass_state[1];
            }

            if (do_lowpass) {
                state.lowpass_state[0] += lowpass_a * (sample - state.lowpass_state[0]);
                state.lowpass_state[1] += lowpass_a * (state.lowpass_state[0] - state.lowpass_state[1]);
                sample = state.lowpass_state[1];
            }

            if (do_delay && !state.delay_buffer.empty()) {
                const size_t capacity = state.delay_buffer.size();
                const size_t offset = std::min(delay_frames, capacity - 1);
                const size_t read_at = (state.delay_index + capacity - offset) % capacity;
                const float delayed = state.delay_buffer[read_at];
                state.delay_buffer[state.delay_index] = sample + delayed * delay_fb;
                if (++state.delay_index >= capacity) state.delay_index = 0;
                sample += delayed * delay_amount;
            }

            if (do_reverb) {
                const float reverb_input = sample * kFixedGain;
                float tail = 0.0f;
                for (int i = 0; i < kCombCount; ++i) {
                    tail += state.combs[i].process(reverb_input, feedback, damping);
                }
                for (int i = 0; i < kAllpassCount; ++i) {
                    tail = state.allpasses[i].process(tail);
                }
                sample = tail * wet + sample * dry;
            }

            output[sample_index] = sample;
        }
    }
}
