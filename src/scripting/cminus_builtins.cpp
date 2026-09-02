// =============================================================================
//  C-Minus builtin registry
//
//  One table describes every function a script can call. The interpreter
//  dispatches through it and the visual script editor builds its node palette
//  from the same rows, so the palette cannot drift from what the language
//  actually provides - which is exactly what the hand-maintained second list it
//  replaced used to do.
//
//  Overloads are separate entries sharing a name with different arities.
//
//  These were previously an if-else chain inside evaluate(), comparing the
//  callee's name against every builtin in turn on every call on every frame.
//  Beyond the cost, a name that matched nothing fell through and returned 0,
//  so a typo'd call was indistinguishable from a function that returns zero.
// =============================================================================

#include "scripting/cminus_interpreter.hpp"
#include "core/input_map.hpp"
#include "core/engine.hpp"
#include "physics/physics_engine.hpp"
#include "world/actor.hpp"
#include "world/material.hpp"

#include <SDL2/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace CMinus {

namespace {

// Argument accessors. Every builtin goes through these so a type error names the
// function and the position, rather than surfacing as a wrong number downstream.
float num(Interpreter& in, const std::vector<Value>& a, size_t i, int line, const char* fn) {
    return in.require_number(a[i], line, fn);
}

Vector3 vec(Interpreter& in, const std::vector<Value>& a, size_t i, int line, const char* fn) {
    // A vec3 argument accepts three scalars packed by the caller as well, because
    // most of these builtins were originally six-float signatures and existing
    // scripts still call them that way.
    return in.require_vec3(a[i], line, fn);
}

Transform& owner_transform(Interpreter& in) {
    return in.actor_owner->get_actor_transform();
}

// --- Math --------------------------------------------------------------------

Value bi_sin(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(std::sin(num(in, a, 0, line, "sin")));
}
Value bi_cos(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(std::cos(num(in, a, 0, line, "cos")));
}
Value bi_tan(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(std::tan(num(in, a, 0, line, "tan")));
}
Value bi_abs(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(std::fabs(num(in, a, 0, line, "abs")));
}
Value bi_sqrt(Interpreter& in, const std::vector<Value>& a, int line) {
    const float v = num(in, a, 0, line, "sqrt");
    // Reported rather than returning NaN, which would propagate into a transform
    // and only show up as geometry vanishing several frames later.
    if (v < 0.0f) in.error(line, "sqrt of a negative number");
    return Value::number(std::sqrt(v));
}
Value bi_pow(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(std::pow(num(in, a, 0, line, "pow"), num(in, a, 1, line, "pow")));
}
Value bi_floor(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(std::floor(num(in, a, 0, line, "floor")));
}
Value bi_ceil(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(std::ceil(num(in, a, 0, line, "ceil")));
}
Value bi_round(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(std::round(num(in, a, 0, line, "round")));
}
Value bi_atan2(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(std::atan2(num(in, a, 0, line, "atan2"), num(in, a, 1, line, "atan2")));
}
Value bi_min(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(std::min(num(in, a, 0, line, "min"), num(in, a, 1, line, "min")));
}
Value bi_max(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(std::max(num(in, a, 0, line, "max"), num(in, a, 1, line, "max")));
}
Value bi_clamp(Interpreter& in, const std::vector<Value>& a, int line) {
    const float v = num(in, a, 0, line, "clamp");
    const float lo = num(in, a, 1, line, "clamp");
    const float hi = num(in, a, 2, line, "clamp");
    // std::clamp is UB when the bounds are inverted, which a script can easily do.
    if (lo > hi) return Value::number(std::clamp(v, hi, lo));
    return Value::number(std::clamp(v, lo, hi));
}
Value bi_lerp(Interpreter& in, const std::vector<Value>& a, int line) {
    const float x = num(in, a, 0, line, "lerp");
    const float y = num(in, a, 1, line, "lerp");
    return Value::number(x + (y - x) * num(in, a, 2, line, "lerp"));
}
Value bi_smoothstep(Interpreter& in, const std::vector<Value>& a, int line) {
    const float e0 = num(in, a, 0, line, "smoothstep");
    const float e1 = num(in, a, 1, line, "smoothstep");
    if (e0 == e1) return Value::number(0.0f);
    const float t = std::clamp((num(in, a, 2, line, "smoothstep") - e0) / (e1 - e0), 0.0f, 1.0f);
    return Value::number(t * t * (3.0f - 2.0f * t));
}
Value bi_rand(Interpreter&, const std::vector<Value>&, int) {
    return Value::number(static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX));
}
Value bi_noise3d(Interpreter& in, const std::vector<Value>& a, int line) {
    const float x = num(in, a, 0, line, "noise3d");
    const float y = num(in, a, 1, line, "noise3d");
    const float z = num(in, a, 2, line, "noise3d");
    const float v = std::sin(x * 12.9898f + y * 78.233f + z * 37.719f) * 43758.5453f;
    return Value::number(v - std::floor(v));
}

// --- Vectors -----------------------------------------------------------------

Value bi_vec3(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::vec(num(in, a, 0, line, "vec3"), num(in, a, 1, line, "vec3"),
                      num(in, a, 2, line, "vec3"));
}
Value bi_length(Interpreter& in, const std::vector<Value>& a, int line) {
    const Vector3 v = vec(in, a, 0, line, "length");
    return Value::number(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
}
Value bi_normalize(Interpreter& in, const std::vector<Value>& a, int line) {
    const Vector3 v = vec(in, a, 0, line, "normalize");
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    // A zero vector has no direction; returning it unchanged is the only answer
    // that does not introduce a NaN.
    if (len < 1e-8f) return Value::vec(0.0f, 0.0f, 0.0f);
    return Value::vec(v.x / len, v.y / len, v.z / len);
}
Value bi_dot_vec(Interpreter& in, const std::vector<Value>& a, int line) {
    const Vector3 u = vec(in, a, 0, line, "dot");
    const Vector3 v = vec(in, a, 1, line, "dot");
    return Value::number(u.x * v.x + u.y * v.y + u.z * v.z);
}
Value bi_cross(Interpreter& in, const std::vector<Value>& a, int line) {
    const Vector3 u = vec(in, a, 0, line, "cross");
    const Vector3 v = vec(in, a, 1, line, "cross");
    return Value::vec(u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x);
}
Value bi_distance_vec(Interpreter& in, const std::vector<Value>& a, int line) {
    const Vector3 u = vec(in, a, 0, line, "distance");
    const Vector3 v = vec(in, a, 1, line, "distance");
    const float dx = u.x - v.x, dy = u.y - v.y, dz = u.z - v.z;
    return Value::number(std::sqrt(dx * dx + dy * dy + dz * dz));
}

// Six-scalar forms, kept because existing scripts predate the vec3 type.
Value bi_distance6(Interpreter& in, const std::vector<Value>& a, int line) {
    const float dx = num(in, a, 0, line, "distance") - num(in, a, 3, line, "distance");
    const float dy = num(in, a, 1, line, "distance") - num(in, a, 4, line, "distance");
    const float dz = num(in, a, 2, line, "distance") - num(in, a, 5, line, "distance");
    return Value::number(std::sqrt(dx * dx + dy * dy + dz * dz));
}
Value bi_dot6(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(num(in, a, 0, line, "dot") * num(in, a, 3, line, "dot") +
                         num(in, a, 1, line, "dot") * num(in, a, 4, line, "dot") +
                         num(in, a, 2, line, "dot") * num(in, a, 5, line, "dot"));
}

// --- Time and input ----------------------------------------------------------

Value bi_get_time(Interpreter& in, const std::vector<Value>&, int) {
    return in.get_variable("time");
}
Value bi_get_dt(Interpreter& in, const std::vector<Value>&, int) {
    return in.get_variable("dt");
}
Value bi_is_key_pressed(Interpreter& in, const std::vector<Value>& a, int line) {
    const int scancode = static_cast<int>(num(in, a, 0, line, "is_key_pressed"));
    if (scancode < 0 || scancode >= SDL_NUM_SCANCODES) return Value::boolean(false);
    const Uint8* state = SDL_GetKeyboardState(nullptr);
    return Value::boolean(state[scancode] != 0);
}
Value bi_action_held(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::boolean(InputMap::get().held(static_cast<int>(num(in, a, 0, line, "action_held"))));
}
Value bi_action_pressed(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::boolean(InputMap::get().pressed(static_cast<int>(num(in, a, 0, line, "action_pressed"))));
}
Value bi_action_released(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::boolean(InputMap::get().released(static_cast<int>(num(in, a, 0, line, "action_released"))));
}
Value bi_action_axis(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(InputMap::get().axis(static_cast<int>(num(in, a, 0, line, "action_axis"))));
}
Value bi_lock_mouse(Interpreter& in, const std::vector<Value>& a, int line) {
    SDL_SetRelativeMouseMode(num(in, a, 0, line, "lock_mouse") > 0.5f ? SDL_TRUE : SDL_FALSE);
    return Value::nothing();
}
Value bi_get_mouse_dx(Interpreter&, const std::vector<Value>&, int) {
    return Value::number(InputMap::get().mouse_dx());
}
Value bi_get_mouse_dy(Interpreter&, const std::vector<Value>&, int) {
    return Value::number(InputMap::get().mouse_dy());
}

// --- Owning actor ------------------------------------------------------------

Value bi_get_position(Interpreter& in, const std::vector<Value>&, int) {
    const DVector3 p = owner_transform(in).position;
    return Value::vec(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
}
Value bi_get_x(Interpreter& in, const std::vector<Value>&, int) {
    return Value::number(static_cast<float>(owner_transform(in).position.x));
}
Value bi_get_y(Interpreter& in, const std::vector<Value>&, int) {
    return Value::number(static_cast<float>(owner_transform(in).position.y));
}
Value bi_get_z(Interpreter& in, const std::vector<Value>&, int) {
    return Value::number(static_cast<float>(owner_transform(in).position.z));
}
Value bi_get_rotation(Interpreter& in, const std::vector<Value>&, int) {
    const Vector3 r = owner_transform(in).rotation;
    return Value::vec(r.x, r.y, r.z);
}
Value bi_get_rot_x(Interpreter& in, const std::vector<Value>&, int) {
    return Value::number(owner_transform(in).rotation.x);
}
Value bi_get_rot_y(Interpreter& in, const std::vector<Value>&, int) {
    return Value::number(owner_transform(in).rotation.y);
}
Value bi_get_rot_z(Interpreter& in, const std::vector<Value>&, int) {
    return Value::number(owner_transform(in).rotation.z);
}

Value bi_set_position3(Interpreter& in, const std::vector<Value>& a, int line) {
    owner_transform(in).position = DVector3{num(in, a, 0, line, "set_position"),
                                            num(in, a, 1, line, "set_position"),
                                            num(in, a, 2, line, "set_position")};
    return Value::nothing();
}
Value bi_set_position_v(Interpreter& in, const std::vector<Value>& a, int line) {
    const Vector3 p = vec(in, a, 0, line, "set_position");
    owner_transform(in).position = DVector3{p.x, p.y, p.z};
    return Value::nothing();
}
Value bi_translate3(Interpreter& in, const std::vector<Value>& a, int line) {
    Transform& t = owner_transform(in);
    t.position.x += num(in, a, 0, line, "translate");
    t.position.y += num(in, a, 1, line, "translate");
    t.position.z += num(in, a, 2, line, "translate");
    return Value::nothing();
}
Value bi_translate_v(Interpreter& in, const std::vector<Value>& a, int line) {
    const Vector3 d = vec(in, a, 0, line, "translate");
    Transform& t = owner_transform(in);
    t.position.x += d.x; t.position.y += d.y; t.position.z += d.z;
    return Value::nothing();
}
Value bi_set_rotation3(Interpreter& in, const std::vector<Value>& a, int line) {
    owner_transform(in).rotation = Vector3(num(in, a, 0, line, "set_rotation"),
                                           num(in, a, 1, line, "set_rotation"),
                                           num(in, a, 2, line, "set_rotation"));
    return Value::nothing();
}
Value bi_set_rotation_v(Interpreter& in, const std::vector<Value>& a, int line) {
    owner_transform(in).rotation = vec(in, a, 0, line, "set_rotation");
    return Value::nothing();
}
Value bi_set_scale3(Interpreter& in, const std::vector<Value>& a, int line) {
    owner_transform(in).scale = Vector3(num(in, a, 0, line, "set_scale"),
                                        num(in, a, 1, line, "set_scale"),
                                        num(in, a, 2, line, "set_scale"));
    return Value::nothing();
}
Value bi_set_scale_v(Interpreter& in, const std::vector<Value>& a, int line) {
    owner_transform(in).scale = vec(in, a, 0, line, "set_scale");
    return Value::nothing();
}

Value bi_look_at3(Interpreter& in, const std::vector<Value>& a, int line) {
    Transform& t = owner_transform(in);
    const double dx = num(in, a, 0, line, "look_at") - t.position.x;
    const double dy = num(in, a, 1, line, "look_at") - t.position.y;
    const double dz = num(in, a, 2, line, "look_at") - t.position.z;
    t.rotation.y = static_cast<float>(std::atan2(dx, dz));
    t.rotation.x = static_cast<float>(std::atan2(dy, std::sqrt(dx * dx + dz * dz)));
    return Value::nothing();
}
Value bi_look_at_v(Interpreter& in, const std::vector<Value>& a, int line) {
    const Vector3 target = vec(in, a, 0, line, "look_at");
    Transform& t = owner_transform(in);
    const double dx = target.x - t.position.x;
    const double dy = target.y - t.position.y;
    const double dz = target.z - t.position.z;
    t.rotation.y = static_cast<float>(std::atan2(dx, dz));
    t.rotation.x = static_cast<float>(std::atan2(dy, std::sqrt(dx * dx + dz * dz)));
    return Value::nothing();
}

Value bi_orbit(Interpreter& in, const std::vector<Value>& a, int line) {
    const float t = in.get_variable("time").x * num(in, a, 4, line, "orbit");
    const float radius = num(in, a, 3, line, "orbit");
    owner_transform(in).position = DVector3{num(in, a, 0, line, "orbit") + std::cos(t) * radius,
                                            num(in, a, 1, line, "orbit"),
                                            num(in, a, 2, line, "orbit") + std::sin(t) * radius};
    return Value::nothing();
}
Value bi_oscillate(Interpreter& in, const std::vector<Value>& a, int line) {
    const float t = in.get_variable("time").x * num(in, a, 0, line, "oscillate");
    owner_transform(in).position.y += std::sin(t) * num(in, a, 1, line, "oscillate");
    return Value::nothing();
}
Value bi_spring(Interpreter& in, const std::vector<Value>& a, int line) {
    // The damped-spring acceleration: -k(x - target) - c*v.
    return Value::number(-num(in, a, 3, line, "spring") *
                             (num(in, a, 0, line, "spring") - num(in, a, 1, line, "spring")) -
                         num(in, a, 4, line, "spring") * num(in, a, 2, line, "spring"));
}

// --- Velocity ----------------------------------------------------------------

Value bi_set_velocity3(Interpreter& in, const std::vector<Value>& a, int line) {
    in.actor_owner->velocity = Vector3(num(in, a, 0, line, "set_velocity"),
                                       num(in, a, 1, line, "set_velocity"),
                                       num(in, a, 2, line, "set_velocity"));
    return Value::nothing();
}
Value bi_set_velocity_v(Interpreter& in, const std::vector<Value>& a, int line) {
    in.actor_owner->velocity = vec(in, a, 0, line, "set_velocity");
    return Value::nothing();
}
Value bi_add_velocity3(Interpreter& in, const std::vector<Value>& a, int line) {
    in.actor_owner->velocity.x += num(in, a, 0, line, "add_velocity");
    in.actor_owner->velocity.y += num(in, a, 1, line, "add_velocity");
    in.actor_owner->velocity.z += num(in, a, 2, line, "add_velocity");
    return Value::nothing();
}
Value bi_get_velocity(Interpreter& in, const std::vector<Value>&, int) {
    const Vector3 v = in.actor_owner->velocity;
    return Value::vec(v.x, v.y, v.z);
}
Value bi_get_vel_x(Interpreter& in, const std::vector<Value>&, int) {
    return Value::number(in.actor_owner->velocity.x);
}
Value bi_get_vel_y(Interpreter& in, const std::vector<Value>&, int) {
    return Value::number(in.actor_owner->velocity.y);
}
Value bi_get_vel_z(Interpreter& in, const std::vector<Value>&, int) {
    return Value::number(in.actor_owner->velocity.z);
}
Value bi_set_angular_velocity(Interpreter& in, const std::vector<Value>& a, int line) {
    in.actor_owner->angular_velocity = Vector3(num(in, a, 0, line, "set_angular_velocity"),
                                               num(in, a, 1, line, "set_angular_velocity"),
                                               num(in, a, 2, line, "set_angular_velocity"));
    return Value::nothing();
}
Value bi_stop_motion(Interpreter& in, const std::vector<Value>&, int) {
    in.actor_owner->velocity = Vector3(0.0f, 0.0f, 0.0f);
    in.actor_owner->angular_velocity = Vector3(0.0f, 0.0f, 0.0f);
    return Value::nothing();
}

// apply_force accelerates rather than teleporting. It used to add straight to
// position, which is a displacement, not a force - it ignored frame time entirely,
// so its effect changed with frame rate.
Value bi_apply_force(Interpreter& in, const std::vector<Value>& a, int line) {
    in.actor_owner->velocity.x += num(in, a, 0, line, "apply_force");
    in.actor_owner->velocity.y += num(in, a, 1, line, "apply_force");
    in.actor_owner->velocity.z += num(in, a, 2, line, "apply_force");
    return Value::nothing();
}

// --- Appearance --------------------------------------------------------------

Value bi_set_color(Interpreter& in, const std::vector<Value>& a, int line) {
    in.actor_owner->actor_color = Vector3(num(in, a, 0, line, "set_color"),
                                          num(in, a, 1, line, "set_color"),
                                          num(in, a, 2, line, "set_color"));
    return Value::nothing();
}
Value bi_set_color_v(Interpreter& in, const std::vector<Value>& a, int line) {
    in.actor_owner->actor_color = vec(in, a, 0, line, "set_color");
    return Value::nothing();
}
Value bi_set_emissive(Interpreter& in, const std::vector<Value>& a, int line) {
    in.actor_owner->emissive = num(in, a, 0, line, "set_emissive");
    return Value::nothing();
}
Value bi_set_emission(Interpreter& in, const std::vector<Value>& a, int line) {
    if (in.actor_owner->assigned_material) {
        in.actor_owner->assigned_material->emission = num(in, a, 0, line, "set_emission");
    }
    return Value::nothing();
}
Value bi_set_metallic(Interpreter& in, const std::vector<Value>& a, int line) {
    if (in.actor_owner->assigned_material) {
        in.actor_owner->assigned_material->metallic = num(in, a, 0, line, "set_metallic");
    }
    return Value::nothing();
}
Value bi_set_roughness(Interpreter& in, const std::vector<Value>& a, int line) {
    if (in.actor_owner->assigned_material) {
        in.actor_owner->assigned_material->roughness = num(in, a, 0, line, "set_roughness");
    }
    return Value::nothing();
}

// --- Renderer and world ------------------------------------------------------

Value bi_set_wireframe(Interpreter& in, const std::vector<Value>& a, int line) {
    if (g_engine) g_engine->get_renderer().wireframe_mode = num(in, a, 0, line, "set_wireframe") > 0.5f;
    return Value::nothing();
}
Value bi_set_msaa(Interpreter& in, const std::vector<Value>& a, int line) {
    if (g_engine) g_engine->get_renderer().enable_msaa = num(in, a, 0, line, "set_msaa") > 0.5f;
    return Value::nothing();
}
Value bi_set_lighting(Interpreter& in, const std::vector<Value>& a, int line) {
    if (g_engine) g_engine->get_renderer().enable_ue4_lighting = num(in, a, 0, line, "set_lighting") > 0.5f;
    return Value::nothing();
}
Value bi_set_time_of_day(Interpreter& in, const std::vector<Value>& a, int line) {
    if (g_engine) g_engine->get_renderer().sky_time_override = num(in, a, 0, line, "set_time_of_day");
    return Value::nothing();
}
Value bi_set_cam_pos(Interpreter& in, const std::vector<Value>& a, int line) {
    if (g_engine) g_engine->camera_pos = DVector3{num(in, a, 0, line, "set_cam_pos"),
                                                  num(in, a, 1, line, "set_cam_pos"),
                                                  num(in, a, 2, line, "set_cam_pos")};
    return Value::nothing();
}
Value bi_set_cam_rot(Interpreter& in, const std::vector<Value>& a, int line) {
    if (g_engine) g_engine->camera_rot = Vector3(num(in, a, 0, line, "set_cam_rot"),
                                                 num(in, a, 1, line, "set_cam_rot"), 0.0f);
    return Value::nothing();
}
Value bi_spawn_actor(Interpreter& in, const std::vector<Value>& a, int line) {
    if (g_engine) g_engine->spawn_actor_by_id(static_cast<int>(num(in, a, 0, line, "spawn_actor")));
    return Value::nothing();
}
Value bi_destroy_self(Interpreter& in, const std::vector<Value>&, int) {
    if (g_engine) g_engine->queue_destroy_actor(in.actor_owner);
    return Value::nothing();
}

Value bi_raycast(Interpreter& in, const std::vector<Value>& a, int line) {
    float distance = 0.0f;
    const bool hit = PhysicsEngine::get_instance().raycast(
        num(in, a, 0, line, "raycast"), num(in, a, 1, line, "raycast"),
        num(in, a, 2, line, "raycast"), num(in, a, 3, line, "raycast"),
        num(in, a, 4, line, "raycast"), num(in, a, 5, line, "raycast"),
        num(in, a, 6, line, "raycast"), distance);
    // -1 rather than 0 for a miss: 0 is a legitimate hit distance for something
    // touching the ray's origin.
    return Value::number(hit ? distance : -1.0f);
}

// --- Scene queries -----------------------------------------------------------

Value bi_get_nearby_actors(Interpreter& in, const std::vector<Value>& a, int line) {
    in.nearby_cache.clear();
    if (g_engine) {
        const float radius = num(in, a, 0, line, "get_nearby_actors");
        const float radius_sq = radius * radius;
        const DVector3 origin = owner_transform(in).position;
        for (auto& other : g_engine->get_actors()) {
            if (other.get() == in.actor_owner) continue;
            const DVector3 p = other->get_actor_transform().position;
            const double dx = origin.x - p.x, dy = origin.y - p.y, dz = origin.z - p.z;
            if (dx * dx + dy * dy + dz * dz <= radius_sq) in.nearby_cache.push_back(other.get());
        }
    }
    return Value::number(static_cast<float>(in.nearby_cache.size()));
}

// Same cache, centred on an arbitrary point rather than on the script's own actor.
// A script driving the camera has a player position belonging to no actor at all,
// so an owner-relative query cannot reach what the player is standing next to.
Value bi_find_actors(Interpreter& in, const std::vector<Value>& a, int line) {
    in.nearby_cache.clear();
    if (g_engine) {
        const float cx = num(in, a, 0, line, "find_actors");
        const float cy = num(in, a, 1, line, "find_actors");
        const float cz = num(in, a, 2, line, "find_actors");
        const float radius = num(in, a, 3, line, "find_actors");
        const float radius_sq = radius * radius;
        for (auto& other : g_engine->get_actors()) {
            if (other.get() == in.actor_owner) continue;
            const DVector3 p = other->get_actor_transform().position;
            const float dx = static_cast<float>(p.x) - cx;
            const float dy = static_cast<float>(p.y) - cy;
            const float dz = static_cast<float>(p.z) - cz;
            if (dx * dx + dy * dy + dz * dz <= radius_sq) in.nearby_cache.push_back(other.get());
        }
    }
    return Value::number(static_cast<float>(in.nearby_cache.size()));
}

// Index validation is shared: an out-of-range index is a script bug worth
// reporting, not something to silently ignore into a wrong-looking scene.
Actor* nearby_at(Interpreter& in, const std::vector<Value>& a, size_t i, int line, const char* fn) {
    const int index = static_cast<int>(num(in, a, i, line, fn));
    if (index < 0 || index >= static_cast<int>(in.nearby_cache.size())) {
        in.error(line, std::string(fn) + " index " + std::to_string(index) +
                           " is outside the " + std::to_string(in.nearby_cache.size()) +
                           " actor(s) found by the last query");
    }
    return in.nearby_cache[static_cast<size_t>(index)];
}

Value bi_get_nearby_position(Interpreter& in, const std::vector<Value>& a, int line) {
    const DVector3 p = nearby_at(in, a, 0, line, "get_nearby_position")->get_actor_transform().position;
    return Value::vec(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
}
Value bi_get_nearby_x(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(static_cast<float>(nearby_at(in, a, 0, line, "get_nearby_x")->get_actor_transform().position.x));
}
Value bi_get_nearby_y(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(static_cast<float>(nearby_at(in, a, 0, line, "get_nearby_y")->get_actor_transform().position.y));
}
Value bi_get_nearby_z(Interpreter& in, const std::vector<Value>& a, int line) {
    return Value::number(static_cast<float>(nearby_at(in, a, 0, line, "get_nearby_z")->get_actor_transform().position.z));
}
Value bi_set_nearby_color(Interpreter& in, const std::vector<Value>& a, int line) {
    // The only way for a script to change an actor other than its own. Without it a
    // script can see the whole scene and alter none of it.
    nearby_at(in, a, 0, line, "set_nearby_color")->actor_color =
        Vector3(num(in, a, 1, line, "set_nearby_color"), num(in, a, 2, line, "set_nearby_color"),
                num(in, a, 3, line, "set_nearby_color"));
    return Value::nothing();
}
Value bi_set_nearby_emissive(Interpreter& in, const std::vector<Value>& a, int line) {
    nearby_at(in, a, 0, line, "set_nearby_emissive")->emissive = num(in, a, 1, line, "set_nearby_emissive");
    return Value::nothing();
}

// --- HUD and audio -----------------------------------------------------------

Value bi_hud_pip(Interpreter& in, const std::vector<Value>& a, int line) {
    if (g_engine) {
        const int index = static_cast<int>(num(in, a, 0, line, "hud_pip"));
        if (index >= 0 && index < 8) {
            g_engine->script_hud.pip[index] = num(in, a, 1, line, "hud_pip");
            if (index + 1 > g_engine->script_hud.pip_count) {
                g_engine->script_hud.pip_count = index + 1;
            }
        }
    }
    return Value::nothing();
}
Value bi_hud_vignette(Interpreter& in, const std::vector<Value>& a, int line) {
    if (g_engine) {
        g_engine->script_hud.vignette = std::clamp(num(in, a, 0, line, "hud_vignette"), 0.0f, 1.0f);
        g_engine->script_hud.vignette_color = Vector3(num(in, a, 1, line, "hud_vignette"),
                                                      num(in, a, 2, line, "hud_vignette"),
                                                      num(in, a, 3, line, "hud_vignette"));
    }
    return Value::nothing();
}

// hud_message(index, seconds) shows line `index` of the scene's .strings table.
// Indexed rather than passed as text so the words live with the project, not the
// engine - which is also what makes them translatable.
Value bi_hud_message(Interpreter& in, const std::vector<Value>& a, int line) {
    if (g_engine) {
        const int index = static_cast<int>(num(in, a, 0, line, "hud_message"));
        if (index < 0) {
            g_engine->script_hud.message_index = -1;
            g_engine->script_hud.message_seconds_left = 0.0f;
        } else {
            // Re-issuing the line already on screen refreshes its timer rather than
            // restarting the fade, so a script calling this every frame shows a
            // steady message instead of a flickering one.
            g_engine->script_hud.message_index = index;
            g_engine->script_hud.message_seconds_left =
                std::max(0.0f, num(in, a, 1, line, "hud_message"));
        }
    }
    return Value::nothing();
}

Value bi_play_sound1(Interpreter& in, const std::vector<Value>& a, int line) {
    if (g_engine) g_engine->play_script_sound(static_cast<int>(num(in, a, 0, line, "play_sound")), 1.0f);
    return Value::nothing();
}
Value bi_play_sound2(Interpreter& in, const std::vector<Value>& a, int line) {
    if (g_engine) {
        g_engine->play_script_sound(static_cast<int>(num(in, a, 0, line, "play_sound")),
                                    std::clamp(num(in, a, 1, line, "play_sound"), 0.0f, 1.0f));
    }
    return Value::nothing();
}

Value bi_print(Interpreter&, const std::vector<Value>& a, int) {
    // Variadic, space separated. to_display_string is what the debugger's watch
    // panel uses, so printed output and inspected values read identically.
    std::cout << "[CMinus print]";
    for (const Value& v : a) std::cout << ' ' << v.to_display_string();
    std::cout << std::endl;
    return Value::nothing();
}

// -----------------------------------------------------------------------------

constexpr ValueType kNum = ValueType::Number;
constexpr ValueType kVec = ValueType::Vec3;
constexpr ValueType kVoid = ValueType::Void;

const std::vector<BuiltinInfo> kBuiltins = {
    // name, arity, arg names, return, category, doc, needs_actor, fn
    {"sin", 1, "radians", kNum, "Math", "Sine of an angle in radians.", false, bi_sin},
    {"cos", 1, "radians", kNum, "Math", "Cosine of an angle in radians.", false, bi_cos},
    {"tan", 1, "radians", kNum, "Math", "Tangent of an angle in radians.", false, bi_tan},
    {"abs", 1, "value", kNum, "Math", "Absolute value.", false, bi_abs},
    {"sqrt", 1, "value", kNum, "Math", "Square root. Errors on a negative input.", false, bi_sqrt},
    {"pow", 2, "base,exponent", kNum, "Math", "base raised to exponent.", false, bi_pow},
    {"floor", 1, "value", kNum, "Math", "Largest integer not greater than value.", false, bi_floor},
    {"ceil", 1, "value", kNum, "Math", "Smallest integer not less than value.", false, bi_ceil},
    {"round", 1, "value", kNum, "Math", "Nearest integer.", false, bi_round},
    {"atan2", 2, "y,x", kNum, "Math", "Angle of the vector (x, y), in radians.", false, bi_atan2},
    {"min", 2, "a,b", kNum, "Math", "Smaller of two numbers.", false, bi_min},
    {"max", 2, "a,b", kNum, "Math", "Larger of two numbers.", false, bi_max},
    {"clamp", 3, "value,low,high", kNum, "Math", "Constrains value to [low, high].", false, bi_clamp},
    {"lerp", 3, "from,to,t", kNum, "Math", "Linear blend from->to by t.", false, bi_lerp},
    {"smoothstep", 3, "edge0,edge1,x", kNum, "Math", "Smooth 0..1 ramp between two edges.", false, bi_smoothstep},
    {"rand", 0, "", kNum, "Math", "Random number in [0, 1].", false, bi_rand},
    {"noise3d", 3, "x,y,z", kNum, "Math", "Deterministic value noise in [0, 1].", false, bi_noise3d},

    {"vec3", 3, "x,y,z", kVec, "Vector", "Builds a vector from three numbers.", false, bi_vec3},
    {"length", 1, "v", kNum, "Vector", "Magnitude of a vector.", false, bi_length},
    {"normalize", 1, "v", kVec, "Vector", "Unit vector in the same direction.", false, bi_normalize},
    {"dot", 2, "a,b", kNum, "Vector", "Dot product of two vectors.", false, bi_dot_vec},
    {"dot", 6, "ax,ay,az,bx,by,bz", kNum, "Vector", "Dot product, as six scalars.", false, bi_dot6},
    {"cross", 2, "a,b", kVec, "Vector", "Cross product of two vectors.", false, bi_cross},
    {"distance", 2, "a,b", kNum, "Vector", "Distance between two points.", false, bi_distance_vec},
    {"distance", 6, "ax,ay,az,bx,by,bz", kNum, "Vector", "Distance, as six scalars.", false, bi_distance6},

    {"get_time", 0, "", kNum, "Time", "Seconds since this script started running.", false, bi_get_time},
    {"get_dt", 0, "", kNum, "Time", "Duration of the current frame, in seconds.", false, bi_get_dt},

    {"is_key_pressed", 1, "scancode", kNum, "Input", "True while an SDL scancode is down.", false, bi_is_key_pressed},
    {"action_held", 1, "action", kNum, "Input", "True while a bound action is held.", false, bi_action_held},
    {"action_pressed", 1, "action", kNum, "Input", "True on the frame an action is pressed.", false, bi_action_pressed},
    {"action_released", 1, "action", kNum, "Input", "True on the frame an action is released.", false, bi_action_released},
    {"action_axis", 1, "action", kNum, "Input", "Analogue value of a bound axis.", false, bi_action_axis},
    {"lock_mouse", 1, "locked", kVoid, "Input", "Captures or releases the mouse cursor.", false, bi_lock_mouse},
    {"get_mouse_dx", 0, "", kNum, "Input", "Horizontal mouse movement this frame.", false, bi_get_mouse_dx},
    {"get_mouse_dy", 0, "", kNum, "Input", "Vertical mouse movement this frame.", false, bi_get_mouse_dy},

    {"get_position", 0, "", kVec, "Transform", "This actor's world position.", true, bi_get_position},
    {"get_x", 0, "", kNum, "Transform", "This actor's world X.", true, bi_get_x},
    {"get_y", 0, "", kNum, "Transform", "This actor's world Y.", true, bi_get_y},
    {"get_z", 0, "", kNum, "Transform", "This actor's world Z.", true, bi_get_z},
    {"get_rotation", 0, "", kVec, "Transform", "This actor's rotation, in radians.", true, bi_get_rotation},
    {"get_rot_x", 0, "", kNum, "Transform", "Pitch, in radians.", true, bi_get_rot_x},
    {"get_rot_y", 0, "", kNum, "Transform", "Yaw, in radians.", true, bi_get_rot_y},
    {"get_rot_z", 0, "", kNum, "Transform", "Roll, in radians.", true, bi_get_rot_z},
    {"set_position", 3, "x,y,z", kVoid, "Transform", "Moves this actor to a point.", true, bi_set_position3},
    {"set_position", 1, "position", kVoid, "Transform", "Moves this actor to a point.", true, bi_set_position_v},
    {"translate", 3, "dx,dy,dz", kVoid, "Transform", "Offsets this actor's position.", true, bi_translate3},
    {"translate", 1, "delta", kVoid, "Transform", "Offsets this actor's position.", true, bi_translate_v},
    {"set_rotation", 3, "pitch,yaw,roll", kVoid, "Transform", "Sets rotation, in radians.", true, bi_set_rotation3},
    {"set_rotation", 1, "rotation", kVoid, "Transform", "Sets rotation, in radians.", true, bi_set_rotation_v},
    {"set_scale", 3, "x,y,z", kVoid, "Transform", "Sets this actor's scale.", true, bi_set_scale3},
    {"set_scale", 1, "scale", kVoid, "Transform", "Sets this actor's scale.", true, bi_set_scale_v},
    {"look_at", 3, "x,y,z", kVoid, "Transform", "Turns this actor to face a point.", true, bi_look_at3},
    {"look_at", 1, "target", kVoid, "Transform", "Turns this actor to face a point.", true, bi_look_at_v},
    {"orbit", 5, "cx,cy,cz,radius,speed", kVoid, "Transform", "Circles this actor about a centre.", true, bi_orbit},
    {"oscillate", 2, "speed,amplitude", kVoid, "Transform", "Bobs this actor up and down.", true, bi_oscillate},
    {"spring", 5, "position,target,velocity,stiffness,damping", kNum, "Math", "Damped-spring acceleration toward a target.", false, bi_spring},

    {"set_velocity", 3, "x,y,z", kVoid, "Physics", "Sets this actor's velocity.", true, bi_set_velocity3},
    {"set_velocity", 1, "velocity", kVoid, "Physics", "Sets this actor's velocity.", true, bi_set_velocity_v},
    {"add_velocity", 3, "x,y,z", kVoid, "Physics", "Adds to this actor's velocity.", true, bi_add_velocity3},
    {"apply_force", 3, "x,y,z", kVoid, "Physics", "Accelerates this actor.", true, bi_apply_force},
    {"get_velocity", 0, "", kVec, "Physics", "This actor's velocity.", true, bi_get_velocity},
    {"get_vel_x", 0, "", kNum, "Physics", "Velocity along X.", true, bi_get_vel_x},
    {"get_vel_y", 0, "", kNum, "Physics", "Velocity along Y.", true, bi_get_vel_y},
    {"get_vel_z", 0, "", kNum, "Physics", "Velocity along Z.", true, bi_get_vel_z},
    {"set_angular_velocity", 3, "x,y,z", kVoid, "Physics", "Sets this actor's spin.", true, bi_set_angular_velocity},
    {"stop_motion", 0, "", kVoid, "Physics", "Zeroes velocity and spin.", true, bi_stop_motion},
    {"raycast", 7, "ox,oy,oz,dx,dy,dz,distance", kNum, "Physics", "Distance to the first hit, or -1 for a miss.", false, bi_raycast},

    {"set_color", 3, "r,g,b", kVoid, "Appearance", "Sets this actor's colour.", true, bi_set_color},
    {"set_color", 1, "color", kVoid, "Appearance", "Sets this actor's colour.", true, bi_set_color_v},
    {"set_emissive", 1, "amount", kVoid, "Appearance", "Sets this actor's emissive strength.", true, bi_set_emissive},
    {"set_emission", 1, "amount", kVoid, "Appearance", "Sets the material's emission.", true, bi_set_emission},
    {"set_metallic", 1, "amount", kVoid, "Appearance", "Sets the material's metallic value.", true, bi_set_metallic},
    {"set_roughness", 1, "amount", kVoid, "Appearance", "Sets the material's roughness.", true, bi_set_roughness},

    {"set_wireframe", 1, "enabled", kVoid, "Renderer", "Toggles wireframe rendering.", false, bi_set_wireframe},
    {"set_msaa", 1, "enabled", kVoid, "Renderer", "Toggles multisampling.", false, bi_set_msaa},
    {"set_lighting", 1, "enabled", kVoid, "Renderer", "Toggles the PBR lighting model.", false, bi_set_lighting},
    {"set_time_of_day", 1, "hours", kVoid, "Renderer", "Overrides the sky's time of day.", false, bi_set_time_of_day},
    {"set_cam_pos", 3, "x,y,z", kVoid, "Camera", "Moves the camera.", false, bi_set_cam_pos},
    {"set_cam_rot", 2, "pitch,yaw", kVoid, "Camera", "Aims the camera.", false, bi_set_cam_rot},

    {"spawn_actor", 1, "id", kVoid, "World", "Spawns an actor by template id.", false, bi_spawn_actor},
    {"destroy_self", 0, "", kVoid, "World", "Removes this actor at the end of the frame.", true, bi_destroy_self},
    {"get_nearby_actors", 1, "radius", kNum, "World", "Finds actors near this one; returns the count.", true, bi_get_nearby_actors},
    {"find_actors", 4, "x,y,z,radius", kNum, "World", "Finds actors near a point; returns the count.", false, bi_find_actors},
    {"get_nearby_position", 1, "index", kVec, "World", "Position of a found actor.", false, bi_get_nearby_position},
    {"get_nearby_x", 1, "index", kNum, "World", "X of a found actor.", false, bi_get_nearby_x},
    {"get_nearby_y", 1, "index", kNum, "World", "Y of a found actor.", false, bi_get_nearby_y},
    {"get_nearby_z", 1, "index", kNum, "World", "Z of a found actor.", false, bi_get_nearby_z},
    {"set_nearby_color", 4, "index,r,g,b", kVoid, "World", "Recolours a found actor.", false, bi_set_nearby_color},
    {"set_nearby_emissive", 2, "index,amount", kVoid, "World", "Sets a found actor's emissive strength.", false, bi_set_nearby_emissive},

    {"hud_pip", 2, "index,value", kVoid, "HUD", "Sets one of the eight HUD pips.", false, bi_hud_pip},
    {"hud_vignette", 4, "amount,r,g,b", kVoid, "HUD", "Tints the screen edges.", false, bi_hud_vignette},
    {"hud_message", 2, "index,seconds", kVoid, "HUD", "Shows a line from the scene's string table.", false, bi_hud_message},
    {"play_sound", 1, "index", kVoid, "Audio", "Plays a one-shot from the scene's sound manifest.", false, bi_play_sound1},
    {"play_sound", 2, "index,volume", kVoid, "Audio", "Plays a one-shot at a given volume.", false, bi_play_sound2},

    {"print", -1, "values...", kVoid, "Debug", "Prints its arguments to the log.", false, bi_print},
};

} // namespace

const std::vector<BuiltinInfo>& builtin_table() { return kBuiltins; }

const BuiltinInfo* find_builtin(const std::string& name, int argc) {
    // Exact arity wins over a variadic entry of the same name, so a name can offer
    // both fixed overloads and a catch-all without the catch-all shadowing them.
    const BuiltinInfo* variadic = nullptr;
    for (const BuiltinInfo& info : kBuiltins) {
        if (name != info.name) continue;
        if (info.arity == argc) return &info;
        if (info.arity == -1) variadic = &info;
    }
    return variadic;
}

bool builtin_name_exists(const std::string& name) {
    for (const BuiltinInfo& info : kBuiltins) {
        if (name == info.name) return true;
    }
    return false;
}

} // namespace CMinus
