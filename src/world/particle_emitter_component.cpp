#include "world/particle_emitter_component.hpp"
#include "world/actor.hpp"
#include "physics/physics_engine.hpp"
#include "core/engine.hpp"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

Vector3 lerp(const Vector3& a, const Vector3& b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
}

} // namespace

const char* ParticleEmitterComponent::shape_name(int shape) {
    switch (shape) {
        case Shape_Point:      return "Point";
        case Shape_Sphere:     return "Sphere";
        case Shape_Hemisphere: return "Hemisphere";
        case Shape_Box:        return "Box";
        case Shape_Cone:       return "Cone";
        case Shape_Circle:     return "Circle";
        default:               return "Unknown";
    }
}

ParticleEmitterComponent::ParticleEmitterComponent(Actor* owner, const std::string& name)
    : SceneComponent(owner, name) {}

float ParticleEmitterComponent::next_random() {
    // xorshift32. Deterministic, per emitter, and far cheaper than rand() - which is
    // also process-global and would make every emitter in the scene correlated.
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return static_cast<float>(random_state & 0x00FFFFFFu) / 16777216.0f;
}

Vector3 ParticleEmitterComponent::to_world(const Vector3& simulation_position) const {
    if (simulation_space == Space_World) return simulation_position;
    // Local space: particles are stored relative to the emitter and follow it.
    const DVector3& origin = transform.position;
    return { simulation_position.x + static_cast<float>(origin.x),
             simulation_position.y + static_cast<float>(origin.y),
             simulation_position.z + static_cast<float>(origin.z) };
}

void ParticleEmitterComponent::sample_shape(Vector3& out_offset, Vector3& out_direction) {
    out_offset = { 0.0f, 0.0f, 0.0f };
    out_direction = { 0.0f, 1.0f, 0.0f };

    switch (shape) {
        case Shape_Point:
            break;

        case Shape_Sphere:
        case Shape_Hemisphere: {
            // Uniform on the sphere: taking z uniformly and the angle uniformly is
            // what avoids the clustering at the poles that naive angle pairs give.
            const float z = (shape == Shape_Hemisphere) ? next_random()
                                                        : (next_random() * 2.0f - 1.0f);
            const float angle = next_random() * 2.0f * kPi;
            const float radial = std::sqrt(std::max(0.0f, 1.0f - z * z));
            out_direction = { radial * std::cos(angle), z, radial * std::sin(angle) };
            out_offset = out_direction * shape_radius;
            break;
        }

        case Shape_Box:
            out_offset = { (next_random() * 2.0f - 1.0f) * shape_extents.x,
                           (next_random() * 2.0f - 1.0f) * shape_extents.y,
                           (next_random() * 2.0f - 1.0f) * shape_extents.z };
            out_direction = { 0.0f, 1.0f, 0.0f };
            break;

        case Shape_Cone: {
            // Spread around +y by up to cone_angle. sqrt on the radius keeps the
            // distribution even across the cone's cross-section instead of bunching
            // it up the middle.
            const float half_angle = std::max(0.0f, cone_angle) * kDegToRad;
            const float around = next_random() * 2.0f * kPi;
            const float tilt = std::sqrt(next_random()) * half_angle;
            const float sin_tilt = std::sin(tilt);
            out_direction = { sin_tilt * std::cos(around), std::cos(tilt), sin_tilt * std::sin(around) };
            const float radial = std::sqrt(next_random()) * shape_radius;
            out_offset = { radial * std::cos(around), 0.0f, radial * std::sin(around) };
            break;
        }

        case Shape_Circle: {
            const float around = next_random() * 2.0f * kPi;
            out_offset = { std::cos(around) * shape_radius, 0.0f, std::sin(around) * shape_radius };
            // Outward, which is what makes a shockwave expand rather than rise.
            out_direction = Vector3{ out_offset.x, 0.0f, out_offset.z }.normalized();
            if (out_direction.length() < 0.5f) out_direction = { 1.0f, 0.0f, 0.0f };
            break;
        }

        default:
            break;
    }
}

void ParticleEmitterComponent::spawn_one(const Vector3& origin_offset) {
    if (static_cast<int>(particles.size()) >= std::max(1, max_particles)) return;

    Vector3 offset, direction;
    sample_shape(offset, direction);

    Particle p;
    // World-space emitters store absolute positions so the particle stays put when
    // the emitter moves; local ones store the offset and are transformed at draw.
    const Vector3 base = (simulation_space == Space_World)
        ? Vector3{ static_cast<float>(transform.position.x),
                   static_cast<float>(transform.position.y),
                   static_cast<float>(transform.position.z) }
        : Vector3{ 0.0f, 0.0f, 0.0f };
    p.position = base + offset + origin_offset;

    const float speed = speed_min + next_random() * std::max(0.0f, speed_max - speed_min);
    p.velocity = direction * speed;
    p.max_life = std::max(0.01f, lifetime_min + next_random() * std::max(0.0f, lifetime_max - lifetime_min));
    p.life = 0.0f;
    p.size = size_min + next_random() * std::max(0.0f, size_max - size_min);
    p.rotation = next_random() * 2.0f * kPi;
    p.angular_velocity = rotation_speed_min + next_random() * (rotation_speed_max - rotation_speed_min);
    p.color = start_color;
    p.seed = next_random();
    particles.push_back(p);
}

void ParticleEmitterComponent::emit_burst_at(const Vector3& world_position, int count) {
    // The burst is expressed as an offset from wherever this emitter's shape would
    // have placed the particle, so shape and burst compose rather than fight.
    const Vector3 emitter_origin = { static_cast<float>(transform.position.x),
                                     static_cast<float>(transform.position.y),
                                     static_cast<float>(transform.position.z) };
    const Vector3 offset = (simulation_space == Space_World)
        ? (world_position - emitter_origin)
        : (world_position - emitter_origin);

    for (int i = 0; i < count; ++i) spawn_one(offset);
}

void ParticleEmitterComponent::trigger_sub_emitter(int trigger, const Vector3& world_position) {
    if (sub_emitter_trigger != trigger || sub_emitter_actor.empty() || sub_emitter_count <= 0) return;
    if (!g_engine) return;

    for (auto& actor : g_engine->get_actors()) {
        if (!actor || actor->get_name() != sub_emitter_actor) continue;
        if (auto* emitter = actor->get_component<ParticleEmitterComponent>()) {
            // Guarding against self-reference: an emitter that sub-emits into itself
            // on death is an unbounded feedback loop.
            if (emitter != this) emitter->emit_burst_at(world_position, sub_emitter_count);
        }
        break;
    }
}

void ParticleEmitterComponent::tick(float delta_time) {
    if (delta_time <= 0.0f) return;

    // --- Emission ----------------------------------------------------------
    if (is_emitting && emit_rate > 0.0f) {
        emit_accumulator += delta_time * emit_rate;
        // A whole number of particles per frame, carrying the remainder forward. The
        // previous code emitted at most one particle per frame however high the rate
        // was set, which quietly capped every emitter at the frame rate.
        int to_spawn = static_cast<int>(emit_accumulator);
        emit_accumulator -= static_cast<float>(to_spawn);
        // Bounded so a long stall cannot try to spawn thousands at once.
        to_spawn = std::min(to_spawn, 256);
        for (int i = 0; i < to_spawn; ++i) spawn_one({ 0.0f, 0.0f, 0.0f });
    }

    if (is_emitting && burst_count > 0 && burst_interval > 0.0f) {
        burst_timer += delta_time;
        while (burst_timer >= burst_interval) {
            burst_timer -= burst_interval;
            for (int i = 0; i < burst_count; ++i) spawn_one({ 0.0f, 0.0f, 0.0f });
        }
    }

    // --- Simulation --------------------------------------------------------
    PhysicsEngine& physics = PhysicsEngine::get_instance();
    const bool can_collide = collision_enabled && physics.is_ready();
    // Shed a fixed fraction of speed per second. The exponential form is what keeps
    // the result the same whether the frame took 4 ms or 40.
    const float drag_factor = std::exp(-std::max(0.0f, drag) * delta_time);

    for (size_t i = 0; i < particles.size();) {
        Particle& p = particles[i];
        p.life += delta_time;

        if (p.life >= p.max_life) {
            trigger_sub_emitter(Sub_Death, to_world(p.position));
            // Swap-and-pop: order does not matter to a particle system, and erasing
            // from the middle of a thousand-element vector every frame does.
            particles[i] = particles.back();
            particles.pop_back();
            continue;
        }

        p.velocity.y -= gravity * delta_time;
        p.velocity += acceleration * delta_time;
        p.velocity *= drag_factor;
        p.rotation += p.angular_velocity * delta_time;

        const Vector3 step = p.velocity * delta_time;

        if (can_collide) {
            const float travel = step.length();
            if (travel > 1e-5f) {
                const Vector3 world_position = to_world(p.position);
                RaycastHit hit;
                if (physics.raycast(DVector3{ world_position.x, world_position.y, world_position.z },
                                    step / travel, travel, hit, collision_layer_mask)) {
                    trigger_sub_emitter(Sub_Collision, hit.point);
                    if (die_on_collision) {
                        particles[i] = particles.back();
                        particles.pop_back();
                        continue;
                    }
                    // Reflect about the surface normal and lose energy. Nudged off
                    // the surface so the next frame's ray does not start inside it
                    // and immediately report another hit.
                    const float into = Vector3::dot(p.velocity, hit.normal);
                    p.velocity = (p.velocity - hit.normal * (2.0f * into)) * std::max(0.0f, collision_bounce);
                    p.position = p.position + (hit.point - world_position) + hit.normal * 0.01f;
                    ++i;
                    continue;
                }
            }
        }

        p.position += step;
        ++i;
    }
}
