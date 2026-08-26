#pragma once

#include "world/component.hpp"
#include <cstdint>
#include <string>
#include <vector>

// One live particle.
//
// Everything that varies over a particle's life is derived from `life` and the
// emitter's settings rather than stored per particle, so this stays small: a
// hundred thousand of these is a few megabytes, and the whole array is walked every
// frame.
struct Particle {
    Vector3 position;
    Vector3 velocity;
    Vector3 color;
    float life = 0.0f;
    float max_life = 1.0f;
    float size = 1.0f;
    float rotation = 0.0f;
    float angular_velocity = 0.0f;
    // Fixed per particle, so per-particle variation is stable rather than flickering
    // frame to frame.
    float seed = 0.0f;
};

// A particle emitter.
//
// Spawns particles in a shape, gives them a randomised initial state, and evolves
// size, colour and alpha along a curve as they age. Particles can collide with the
// physics world, and can trigger a burst on another emitter when they die or hit
// something - which is how a rocket's smoke trail turns into an explosion.
class ParticleEmitterComponent : public SceneComponent {
public:
    // Where new particles start and which way they head. Numbering is part of the
    // scene file format, so new shapes append.
    enum EmissionShape : int {
        Shape_Point      = 0,
        Shape_Sphere     = 1,
        // Upper half of a sphere. The right shape for something rising off a
        // surface - steam, dust - where a full sphere sends half of it into the floor.
        Shape_Hemisphere = 2,
        Shape_Box        = 3,
        Shape_Cone       = 4,
        // Flat ring in the xz plane, for shockwaves and ground rings.
        Shape_Circle     = 5,
        Shape_Count      = 6
    };

    enum SimulationSpace : int {
        // Particles move with the emitter. A torch flame carried by a character.
        Space_Local = 0,
        // Particles are left behind. Smoke trails, exhaust.
        Space_World = 1
    };

    enum BlendMode : int {
        // Light-emitting: fire, sparks, magic. Never darkens what is behind it.
        Blend_Additive = 0,
        // Light-occluding: smoke, dust, steam.
        Blend_Alpha    = 1
    };

    enum SubEmitterTrigger : int {
        Sub_None      = 0,
        Sub_Death     = 1,
        Sub_Collision = 2
    };

    static const char* shape_name(int shape);

    ParticleEmitterComponent(Actor* owner, const std::string& name);

    void tick(float delta_time) override;

    // --- Emission ------------------------------------------------------------
    bool is_emitting = true;
    float emit_rate = 30.0f;          // particles per second
    // Extra particles released together every burst_interval seconds. Zero count
    // disables it. This is what makes a pulse rather than a stream.
    int burst_count = 0;
    float burst_interval = 1.0f;
    // Hard ceiling. A long lifetime and a high rate multiply, and without this an
    // emitter left running quietly eats the frame budget.
    int max_particles = 1000;
    int simulation_space = Space_World;

    // --- Shape ---------------------------------------------------------------
    int shape = Shape_Cone;
    float shape_radius = 0.5f;
    Vector3 shape_extents = { 0.5f, 0.5f, 0.5f };
    // Half-angle of the cone, in degrees. Zero is a straight beam.
    float cone_angle = 25.0f;

    // --- Initial state -------------------------------------------------------
    float lifetime_min = 1.0f;
    float lifetime_max = 2.0f;
    float speed_min = 2.0f;
    float speed_max = 4.0f;
    float size_min = 0.15f;
    float size_max = 0.3f;
    float rotation_speed_min = -2.0f;
    float rotation_speed_max = 2.0f;
    Vector3 start_color = { 1.0f, 0.6f, 0.15f };
    float start_alpha = 1.0f;

    // --- Over lifetime -------------------------------------------------------
    // Colour and alpha interpolate from the start values to these as a particle
    // ages. Fire goes orange to dark red to nothing; smoke goes grey to transparent.
    Vector3 end_color = { 0.6f, 0.1f, 0.0f };
    float end_alpha = 0.0f;
    // Size multiplier at birth and at death, so a puff can grow and a spark shrink.
    float size_start_scale = 1.0f;
    float size_end_scale = 0.2f;
    // Downward acceleration. Negative makes particles rise, which is what smoke does.
    float gravity = 1.5f;
    // Fraction of velocity shed per second. This is what makes a puff slow and hang
    // rather than flying off forever.
    float drag = 0.6f;
    // Constant acceleration in the simulation space - wind, an updraught.
    Vector3 acceleration = { 0.0f, 0.0f, 0.0f };

    // --- Rendering -----------------------------------------------------------
    int blend_mode = Blend_Additive;
    // Multiplied into the particle colour. Empty draws a soft round dot generated in
    // the shader, so an emitter is usable before any texture exists.
    std::string texture_path;
    // Brightness multiplier. Particles are drawn as emissive light, so this is how
    // a spark reads as hot rather than as a beige square.
    float intensity = 2.0f;

    // --- Collision -----------------------------------------------------------
    // Particles are swept against the physics world each step. Off by default: it
    // costs a raycast per particle per frame and most emitters do not need it.
    bool collision_enabled = false;
    // Fraction of speed kept when bouncing. Zero makes particles stick and slide.
    float collision_bounce = 0.35f;
    // Layers particles collide with, as a bit mask.
    uint32_t collision_layer_mask = 0xFFFFFFFFu;
    bool die_on_collision = false;

    // --- Sub-emitter ---------------------------------------------------------
    // Name of another actor carrying a ParticleEmitterComponent. When this emitter's
    // particles die or hit something, that emitter fires a burst at the spot.
    std::string sub_emitter_actor;
    int sub_emitter_trigger = Sub_None;
    int sub_emitter_count = 8;

    // Releases `count` particles at a world-space position, ignoring this emitter's
    // own shape origin. How a sub-emitter is triggered, and useful directly from a
    // script for a one-off effect.
    void emit_burst_at(const Vector3& world_position, int count);

    const std::vector<Particle>& get_particles() const { return particles; }
    // Age as a 0..1 fraction, which is what every over-lifetime curve is keyed on.
    static float particle_fraction(const Particle& p) {
        return (p.max_life > 1e-6f) ? (p.life / p.max_life) : 1.0f;
    }

private:
    void spawn_one(const Vector3& origin_offset);
    // Initial direction and speed for the configured shape, in simulation space.
    void sample_shape(Vector3& out_offset, Vector3& out_direction);
    // Fires the configured sub-emitter, if there is one, at a world position.
    void trigger_sub_emitter(int trigger, const Vector3& world_position);
    // Simulation-space position to world space, which differ only in Local mode.
    Vector3 to_world(const Vector3& simulation_position) const;

    std::vector<Particle> particles;
    float emit_accumulator = 0.0f;
    float burst_timer = 0.0f;
    // Advanced per spawn so successive particles differ. A plain rand() would tie
    // every emitter in the scene to one shared sequence.
    unsigned int random_state = 0x1234567u;
    float next_random();
};
