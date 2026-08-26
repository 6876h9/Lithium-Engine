#pragma once

#include "component.hpp"
#include "audio/audio_engine.hpp"
#include <string>
#include <memory>

class AudioComponent : public ActorComponent {
public:
    AudioComponent(Actor* owner, const std::string& name);
    ~AudioComponent() override;

    void tick(float delta_time) override;

    void play();
    void stop();
    void pause();
    bool is_playing();

    void set_file_path(const std::string& path);
    const std::string& get_file_path() const { return file_path; }

    void set_looping(bool loop);
    bool get_looping() const { return is_looping; }

    void set_spatial(bool spatial);
    bool get_spatial() const { return is_spatial; }

    void set_volume(float vol);
    float get_volume() const { return volume; }

    void set_pitch(float p);
    float get_pitch() const { return pitch; }

    // --- Routing -------------------------------------------------------------
    // Mixer bus this sound plays into. Changing it re-creates the sound, because
    // miniaudio binds a sound to its group at creation.
    int bus = 0;

    // --- Spatialisation ------------------------------------------------------
    // Pitch shift from relative motion between this source and the listener. Zero
    // disables it; 1 is physically correct. Needs the listener velocity to be fed
    // to AudioEngine::set_listener_velocity, or there is no relative motion to
    // shift by.
    float doppler_factor = 0.0f;
    // Full volume within min_distance, and no further attenuation past max_distance.
    float min_distance = 1.0f;
    float max_distance = 60.0f;
    // How sharply volume falls between those two. 1 is inverse-distance.
    float rolloff = 1.0f;
    // 0 none, 1 inverse, 2 linear, 3 exponential. Matches miniaudio's enum order.
    int attenuation_model = 1;
    // Directional source. Inside the inner angle the sound is at full volume,
    // outside the outer angle it drops to cone_outer_gain, and it fades between.
    // Both at 360 degrees is an omnidirectional source, which is the default.
    float cone_inner_angle = 360.0f;
    float cone_outer_angle = 360.0f;
    float cone_outer_gain = 0.25f;

    // Pushes every spatial setting above onto the live sound.
    void apply_spatial_settings();

    // Re-creates the sound from the current file and bus. Public because changing
    // the bus requires it: miniaudio binds a sound to its group at creation.
    void reload_sound();

private:


    std::string file_path = "";
    bool is_looping = false;
    bool is_spatial = true;
    float volume = 1.0f;
    float pitch = 1.0f;


    ma_sound sound;
    bool sound_initialized = false;
    // Previous frame's position, so velocity can be differentiated for doppler.
    Vector3 previous_position;
    bool has_previous_position = false;
};
