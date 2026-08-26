#pragma once

#include "miniaudio.h"
#include "audio/audio_bus.hpp"
#include "core/math.hpp" // For Vector3
#include <string>

class AudioEngine {
public:
    // Mixer buses. Every sound plays into one, and each has its own volume and DSP
    // chain - which is how "duck the music while someone is talking" or "muffle
    // everything but the UI while underwater" is expressed as one change instead of
    // one per sound.
    //
    // Four rather than an unbounded list: each carries a reverb with its own filter
    // bank, and a project that genuinely needs more buses than this is past the
    // point where it wants a real mixing desk.
    static constexpr int kBusCount = 4;

    struct BusSettings {
        std::string name;
        float volume = 1.0f;
    };

    static AudioEngine& get() {
        static AudioEngine instance;
        return instance;
    }

    bool init();
    void shutdown();
    void set_listener(const Vector3& position, const Vector3& forward, const Vector3& up);
    // Listener velocity, which is what the doppler shift is computed against. Zero
    // unless something calls this, so doppler does nothing until it is fed.
    void set_listener_velocity(const Vector3& velocity);

    ma_engine* get_engine() { return &engine; }
    // The engine now starts before audio exists (the main menu is up while the
    // heavy subsystems are still uninitialised), so callers that poke at the
    // miniaudio engine have to be able to ask first.
    bool initialized() const { return is_initialized; }

    // --- Buses ---------------------------------------------------------------
    // The group a sound should be created against. Null before init, or for an
    // out-of-range index, in which case the sound plays straight to the endpoint.
    ma_sound_group* get_bus_group(int bus);
    BusSettings& get_bus_settings(int bus);
    // The bus's DSP chain, for the editor to drive. Null before init.
    BusEffectNode* get_bus_effects(int bus);
    // Pushes the settings volume onto the live group.
    void apply_bus_volume(int bus);
    const std::string& get_bus_name(int bus) const;

private:
    AudioEngine() = default;
    ~AudioEngine() = default;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool valid_bus(int bus) const { return bus >= 0 && bus < kBusCount; }

    ma_engine engine;
    bool is_initialized = false;

    // Each bus is a sound group whose output runs through its effect node before
    // reaching the engine endpoint. Wired once at init and never rewired.
    ma_sound_group bus_groups[kBusCount];
    BusEffectNode bus_effects[kBusCount];
    BusSettings bus_settings[kBusCount];
    bool bus_ready[kBusCount] = { false, false, false, false };
};
