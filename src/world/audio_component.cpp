#include "world/audio_component.hpp"
#include "world/actor.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>

AudioComponent::AudioComponent(Actor* owner, const std::string& name) : ActorComponent(owner, name) {
}

AudioComponent::~AudioComponent() {
    if (sound_initialized) {
        ma_sound_uninit(&sound);
    }
}

void AudioComponent::tick(float delta_time) {
    if (!sound_initialized || !is_spatial || !owner) return;

    const Vector3 position = owner->get_actor_transform().position.to_vec3();
    ma_sound_set_position(&sound, position.x, position.y, position.z);

    // Velocity, so a moving source can doppler-shift. Differentiated from the
    // transform rather than read off the actor, because an actor driven by physics,
    // by an animation or by a script all move without ever setting a velocity field.
    if (delta_time > 1e-5f && has_previous_position) {
        const Vector3 velocity = (position - previous_position) * (1.0f / delta_time);
        ma_sound_set_velocity(&sound, velocity.x, velocity.y, velocity.z);
    }
    previous_position = position;
    has_previous_position = true;

    // Cone direction follows the actor's facing, which is what makes a directional
    // source - a speaker, a searchlight hum - point where its actor points.
    if (cone_inner_angle < 359.0f || cone_outer_angle < 359.0f) {
        const float yaw = owner->get_actor_transform().rotation.y;
        // Matches the engine's camera basis, where -Z is forward at zero yaw.
        ma_sound_set_direction(&sound, -std::sin(yaw), 0.0f, -std::cos(yaw));
    }
}

void AudioComponent::apply_spatial_settings() {
    if (!sound_initialized) return;

    ma_sound_set_doppler_factor(&sound, std::max(0.0f, doppler_factor));
    ma_sound_set_min_distance(&sound, std::max(0.01f, min_distance));
    ma_sound_set_max_distance(&sound, std::max(min_distance + 0.01f, max_distance));
    ma_sound_set_rolloff(&sound, std::max(0.0f, rolloff));

    ma_attenuation_model model = ma_attenuation_model_inverse;
    switch (attenuation_model) {
        case 0: model = ma_attenuation_model_none; break;
        case 2: model = ma_attenuation_model_linear; break;
        case 3: model = ma_attenuation_model_exponential; break;
        default: break;
    }
    ma_sound_set_attenuation_model(&sound, model);

    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    ma_sound_set_cone(&sound,
                      std::max(0.0f, cone_inner_angle) * kDegToRad,
                      std::max(cone_inner_angle, cone_outer_angle) * kDegToRad,
                      std::max(0.0f, std::min(1.0f, cone_outer_gain)));
}

void AudioComponent::reload_sound() {
    if (sound_initialized) {
        ma_sound_uninit(&sound);
        sound_initialized = false;
    }

    if (file_path.empty()) return;

    ma_engine* engine = AudioEngine::get().get_engine();
    if (!engine) return;

    // Created against its mixer bus. A null group is the engine endpoint, which is
    // the correct fallback when the bus could not be built.
    ma_sound_group* group = AudioEngine::get().get_bus_group(bus);
    ma_result result = ma_sound_init_from_file(engine, file_path.c_str(), 0, group, NULL, &sound);
    if (result == MA_SUCCESS) {
        sound_initialized = true;
        ma_sound_set_looping(&sound, is_looping ? MA_TRUE : MA_FALSE);
        ma_sound_set_spatialization_enabled(&sound, is_spatial ? MA_TRUE : MA_FALSE);
        ma_sound_set_volume(&sound, volume);
        ma_sound_set_pitch(&sound, pitch);
        apply_spatial_settings();
    } else {
        std::cerr << "Failed to load audio file: " << file_path << std::endl;
    }
}

void AudioComponent::play() {
    if (sound_initialized) {
        ma_sound_start(&sound);
    }
}

void AudioComponent::stop() {
    if (sound_initialized) {
        ma_sound_stop(&sound);
        ma_sound_seek_to_pcm_frame(&sound, 0); // rewind
    }
}

void AudioComponent::pause() {
    if (sound_initialized) {
        ma_sound_stop(&sound);
    }
}

bool AudioComponent::is_playing() {
    if (!sound_initialized) return false;
    return ma_sound_is_playing(&sound) == MA_TRUE;
}

void AudioComponent::set_file_path(const std::string& path) {
    if (file_path != path) {
        file_path = path;
        reload_sound();
    }
}

void AudioComponent::set_looping(bool loop) {
    is_looping = loop;
    if (sound_initialized) {
        ma_sound_set_looping(&sound, is_looping ? MA_TRUE : MA_FALSE);
    }
}

void AudioComponent::set_spatial(bool spatial) {
    is_spatial = spatial;
    if (sound_initialized) {
        ma_sound_set_spatialization_enabled(&sound, is_spatial ? MA_TRUE : MA_FALSE);
    }
}

void AudioComponent::set_volume(float vol) {
    volume = vol;
    if (sound_initialized) {
        ma_sound_set_volume(&sound, volume);
    }
}

void AudioComponent::set_pitch(float p) {
    pitch = p;
    if (sound_initialized) {
        ma_sound_set_pitch(&sound, pitch);
    }
}


