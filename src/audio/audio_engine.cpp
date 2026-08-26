#include "audio/audio_engine.hpp"
#include <iostream>

namespace {
// Sensible starting names. A project renames them in Settings > Audio Buses; the
// engine only ever refers to them by index.
const char* kDefaultBusNames[AudioEngine::kBusCount] = { "Master", "Music", "SFX", "Ambience" };
} // namespace

bool AudioEngine::init() {
    if (is_initialized) return true;

    ma_engine_config engineConfig = ma_engine_config_init();
    // Default listener is 0
    engineConfig.listenerCount = 1;
    
    if (ma_engine_init(&engineConfig, &engine) != MA_SUCCESS) {
        std::cerr << "Failed to initialize miniaudio engine." << std::endl;
        return false;
    }

    is_initialized = true;

    const ma_uint32 channels = ma_engine_get_channels(&engine);
    const ma_uint32 sample_rate = ma_engine_get_sample_rate(&engine);
    ma_node_graph* graph = ma_engine_get_node_graph(&engine);

    for (int bus = 0; bus < kBusCount; ++bus) {
        bus_settings[bus].name = kDefaultBusNames[bus];
        bus_settings[bus].volume = 1.0f;

        if (ma_sound_group_init(&engine, 0, nullptr, &bus_groups[bus]) != MA_SUCCESS) {
            std::cerr << "[Audio] Bus " << bus << " could not be created; sounds on it "
                         "will play unrouted." << std::endl;
            continue;
        }

        if (!bus_effects[bus].initialize(graph, channels, sample_rate)) {
            // The group still works, it just has no effects. Better than losing the
            // bus entirely, and the editor will show the effects as unavailable.
            std::cerr << "[Audio] Bus " << bus << " effect chain unavailable." << std::endl;
            bus_ready[bus] = true;
            continue;
        }

        // group -> effects -> endpoint. Wired once, never rebuilt: editing a live
        // node graph from the main thread races the audio thread walking it.
        ma_node_attach_output_bus(&bus_groups[bus], 0, &bus_effects[bus].base, 0);
        ma_node_attach_output_bus(&bus_effects[bus].base, 0, ma_engine_get_endpoint(&engine), 0);
        bus_ready[bus] = true;
    }

    return true;
}

void AudioEngine::shutdown() {
    if (!is_initialized) return;

    for (int bus = 0; bus < kBusCount; ++bus) {
        if (!bus_ready[bus]) continue;
        // Effects first: the group feeds them, and tearing down the target of a live
        // attachment before its source is what leaves a dangling node behind.
        bus_effects[bus].uninitialize();
        ma_sound_group_uninit(&bus_groups[bus]);
        bus_ready[bus] = false;
    }

    ma_engine_uninit(&engine);
    is_initialized = false;
}

void AudioEngine::set_listener(const Vector3& position, const Vector3& forward, const Vector3& up) {
    if (!is_initialized) return;

    ma_engine_listener_set_position(&engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&engine, 0, up.x, up.y, up.z);
}

void AudioEngine::set_listener_velocity(const Vector3& velocity) {
    if (!is_initialized) return;
    ma_engine_listener_set_velocity(&engine, 0, velocity.x, velocity.y, velocity.z);
}

ma_sound_group* AudioEngine::get_bus_group(int bus) {
    if (!is_initialized || !valid_bus(bus) || !bus_ready[bus]) return nullptr;
    return &bus_groups[bus];
}

AudioEngine::BusSettings& AudioEngine::get_bus_settings(int bus) {
    static BusSettings fallback;
    if (!valid_bus(bus)) return fallback;
    return bus_settings[bus];
}

BusEffectNode* AudioEngine::get_bus_effects(int bus) {
    if (!is_initialized || !valid_bus(bus) || !bus_effects[bus].is_initialized()) return nullptr;
    return &bus_effects[bus];
}

void AudioEngine::apply_bus_volume(int bus) {
    if (!is_initialized || !valid_bus(bus) || !bus_ready[bus]) return;
    ma_sound_group_set_volume(&bus_groups[bus], bus_settings[bus].volume);
}

const std::string& AudioEngine::get_bus_name(int bus) const {
    static const std::string invalid = "Invalid";
    if (!valid_bus(bus)) return invalid;
    return bus_settings[bus].name;
}
