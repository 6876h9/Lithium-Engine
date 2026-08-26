#include "scripting/lua_api.hpp"
#include <algorithm>

#include "world/actor.hpp"
#include "world/collision_event.hpp"
#include "world/static_mesh_component.hpp"
#include "world/animation_player.hpp"
#include "world/character_controller_component.hpp"
#include "world/joint_component.hpp"
#include "world/ui_canvas_component.hpp"
#include "world/nav_agent_component.hpp"
#include "navigation/navmesh.hpp"
#include "core/engine.hpp"
#include "core/input_map.hpp"
#include "physics/physics_engine.hpp"

// Not wrapped in extern "C": Lua is compiled as C++ in this build (see CMakeLists),
// so that an error inside a binding unwinds as an exception instead of longjmp-ing
// past the destructors of the std::strings these functions build. Its symbols are
// therefore C++-mangled and the headers must be included as C++ to match.
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"


#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

namespace {

// Registry key for the actor this VM is currently running for. The address of this
// variable is the key, which is the usual way to get a collision-free registry slot.
const char kCurrentActorKey = 0;

Actor* current_actor(lua_State* L) {
    lua_pushlightuserdata(L, (void*)&kCurrentActorKey);
    lua_gettable(L, LUA_REGISTRYINDEX);
    Actor* actor = static_cast<Actor*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return actor;
}

// Every actor entry point goes through this, so a script that somehow runs without
// an owner reports it instead of dereferencing null.
Actor* checked_actor(lua_State* L) {
    Actor* actor = current_actor(L);
    if (!actor) luaL_error(L, "no actor is bound to this script");
    return actor;
}

float arg_float(lua_State* L, int index) { return static_cast<float>(luaL_checknumber(L, index)); }

// --- Actor: self -----------------------------------------------------------

int l_get_position(lua_State* L) {
    const Transform& t = checked_actor(L)->get_actor_transform();
    lua_pushnumber(L, t.position.x); lua_pushnumber(L, t.position.y); lua_pushnumber(L, t.position.z);
    return 3;
}
int l_set_position(lua_State* L) {
    Transform& t = checked_actor(L)->get_actor_transform();
    t.position = { luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3) };
    return 0;
}
int l_translate(lua_State* L) {
    Transform& t = checked_actor(L)->get_actor_transform();
    t.position.x += luaL_checknumber(L, 1);
    t.position.y += luaL_checknumber(L, 2);
    t.position.z += luaL_checknumber(L, 3);
    return 0;
}
int l_get_rotation(lua_State* L) {
    const Transform& t = checked_actor(L)->get_actor_transform();
    lua_pushnumber(L, t.rotation.x); lua_pushnumber(L, t.rotation.y); lua_pushnumber(L, t.rotation.z);
    return 3;
}
int l_set_rotation(lua_State* L) {
    Transform& t = checked_actor(L)->get_actor_transform();
    t.rotation = { arg_float(L, 1), arg_float(L, 2), arg_float(L, 3) };
    return 0;
}
int l_rotate(lua_State* L) {
    Transform& t = checked_actor(L)->get_actor_transform();
    t.rotation.x += arg_float(L, 1); t.rotation.y += arg_float(L, 2); t.rotation.z += arg_float(L, 3);
    return 0;
}
int l_get_scale(lua_State* L) {
    const Transform& t = checked_actor(L)->get_actor_transform();
    lua_pushnumber(L, t.scale.x); lua_pushnumber(L, t.scale.y); lua_pushnumber(L, t.scale.z);
    return 3;
}
int l_set_scale(lua_State* L) {
    Transform& t = checked_actor(L)->get_actor_transform();
    t.scale = { arg_float(L, 1), arg_float(L, 2), arg_float(L, 3) };
    return 0;
}
int l_get_name(lua_State* L) { lua_pushstring(L, checked_actor(L)->get_name().c_str()); return 1; }

int l_set_color(lua_State* L) {
    checked_actor(L)->actor_color = { arg_float(L, 1), arg_float(L, 2), arg_float(L, 3) };
    return 0;
}
int l_set_metallic(lua_State* L)  { checked_actor(L)->metallic  = arg_float(L, 1); return 0; }
int l_set_roughness(lua_State* L) { checked_actor(L)->roughness = arg_float(L, 1); return 0; }
int l_set_emissive(lua_State* L)  { checked_actor(L)->emissive  = arg_float(L, 1); return 0; }
int l_set_visible(lua_State* L)   { checked_actor(L)->is_invisible = !lua_toboolean(L, 1); return 0; }

int l_get_velocity(lua_State* L) {
    const Vector3& v = checked_actor(L)->velocity;
    lua_pushnumber(L, v.x); lua_pushnumber(L, v.y); lua_pushnumber(L, v.z);
    return 3;
}
int l_set_velocity(lua_State* L) {
    checked_actor(L)->velocity = { arg_float(L, 1), arg_float(L, 2), arg_float(L, 3) };
    return 0;
}
int l_set_angular_velocity(lua_State* L) {
    checked_actor(L)->angular_velocity = { arg_float(L, 1), arg_float(L, 2), arg_float(L, 3) };
    return 0;
}

// --- Input -----------------------------------------------------------------

int l_input_axis(lua_State* L)     { lua_pushnumber(L, InputMap::get().axis(luaL_checkstring(L, 1))); return 1; }
int l_input_held(lua_State* L)     { lua_pushboolean(L, InputMap::get().held(luaL_checkstring(L, 1))); return 1; }
int l_input_pressed(lua_State* L)  { lua_pushboolean(L, InputMap::get().pressed(luaL_checkstring(L, 1))); return 1; }
int l_input_released(lua_State* L) { lua_pushboolean(L, InputMap::get().released(luaL_checkstring(L, 1))); return 1; }
int l_input_mouse_delta(lua_State* L) {
    lua_pushnumber(L, InputMap::get().mouse_dx());
    lua_pushnumber(L, InputMap::get().mouse_dy());
    return 2;
}

// --- World -----------------------------------------------------------------

// Cross-actor access is by name and read-only. Scripts tick in parallel, so handing
// one script a writable reference to another actor would let two threads write the
// same Transform; reads are safe because the actor list is not mutated during the
// tick, and every mutation below is deferred to the logic thread.
Actor* find_actor(const char* name) {
    if (!g_engine || !name) return nullptr;
    for (auto& actor : g_engine->get_actors()) {
        if (actor && actor->get_name() == name) return actor.get();
    }
    return nullptr;
}

int l_world_exists(lua_State* L) { lua_pushboolean(L, find_actor(luaL_checkstring(L, 1)) != nullptr); return 1; }

int l_world_find_position(lua_State* L) {
    Actor* a = find_actor(luaL_checkstring(L, 1));
    if (!a) { lua_pushnil(L); return 1; }
    const Transform& t = a->get_actor_transform();
    lua_pushnumber(L, t.position.x); lua_pushnumber(L, t.position.y); lua_pushnumber(L, t.position.z);
    return 3;
}

int l_world_distance_to(lua_State* L) {
    Actor* other = find_actor(luaL_checkstring(L, 1));
    if (!other) { lua_pushnil(L); return 1; }
    const DVector3& a = checked_actor(L)->get_actor_transform().position;
    const DVector3& b = other->get_actor_transform().position;
    lua_pushnumber(L, (b - a).length());
    return 1;
}

int l_world_actor_count(lua_State* L) {
    lua_pushinteger(L, g_engine ? static_cast<lua_Integer>(g_engine->get_actors().size()) : 0);
    return 1;
}

int l_world_camera_position(lua_State* L) {
    if (!g_engine) { lua_pushnil(L); return 1; }
    const DVector3& p = g_engine->get_camera_position();
    lua_pushnumber(L, p.x); lua_pushnumber(L, p.y); lua_pushnumber(L, p.z);
    return 3;
}

int l_world_spawn(lua_State* L) {
    if (!g_engine) return 0;
    int kind = static_cast<int>(luaL_checkinteger(L, 1));
    double x = luaL_optnumber(L, 2, 0.0), y = luaL_optnumber(L, 3, 0.0), z = luaL_optnumber(L, 4, 0.0);
    // Deferred: adding to the actor list mid-tick would reallocate the vector the
    // task graph is iterating.
    g_engine->script_request_spawn(kind, x, y, z);
    return 0;
}

int l_world_destroy(lua_State* L) {
    // No argument destroys the actor running the script, which is the common case
    // (a pickup removing itself).
    Actor* target = lua_isnoneornil(L, 1) ? checked_actor(L) : find_actor(luaL_checkstring(L, 1));
    if (g_engine && target) g_engine->script_request_destroy(target);
    return 0;
}

// Pushes a hit as a table, which is the only shape that stays readable once a hit
// carries six things. Callers write `if h then print(h.actor, h.distance) end`.
void push_hit(lua_State* L, const RaycastHit& hit) {
    lua_newtable(L);
    if (hit.actor) lua_pushstring(L, hit.actor->get_name().c_str()); else lua_pushnil(L);
    lua_setfield(L, -2, "actor");
    lua_pushnumber(L, hit.distance);   lua_setfield(L, -2, "distance");
    lua_pushnumber(L, hit.point.x);    lua_setfield(L, -2, "x");
    lua_pushnumber(L, hit.point.y);    lua_setfield(L, -2, "y");
    lua_pushnumber(L, hit.point.z);    lua_setfield(L, -2, "z");
    lua_pushnumber(L, hit.normal.x);   lua_setfield(L, -2, "nx");
    lua_pushnumber(L, hit.normal.y);   lua_setfield(L, -2, "ny");
    lua_pushnumber(L, hit.normal.z);   lua_setfield(L, -2, "nz");
    lua_pushinteger(L, hit.layer);     lua_setfield(L, -2, "layer");
}

// Optional trailing layer mask. Omitted means every layer, which is what a script
// that has never heard of layers should get.
uint32_t optional_layer_mask(lua_State* L, int index) {
    if (lua_isnoneornil(L, index)) return 0xFFFFFFFFu;
    return static_cast<uint32_t>(luaL_checkinteger(L, index));
}

// world.raycast(x,y,z, dx,dy,dz [, max_distance [, layer_mask]])
// Returns a hit table, or nil. The old two-value form returned only a distance and
// could not answer what was hit - which is most of what a raycast is for.
int l_world_raycast(lua_State* L) {
    RaycastHit hit;
    const bool did_hit = PhysicsEngine::get_instance().raycast(
        DVector3{ luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3) },
        Vector3{ static_cast<float>(luaL_checknumber(L, 4)),
                 static_cast<float>(luaL_checknumber(L, 5)),
                 static_cast<float>(luaL_checknumber(L, 6)) },
        static_cast<float>(luaL_optnumber(L, 7, 1000.0)), hit, optional_layer_mask(L, 8));
    if (!did_hit) { lua_pushnil(L); return 1; }
    push_hit(L, hit);
    return 1;
}

// world.raycast_all(...) -> array of hit tables, nearest first.
int l_world_raycast_all(lua_State* L) {
    std::vector<RaycastHit> hits;
    PhysicsEngine::get_instance().raycast_all(
        DVector3{ luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3) },
        Vector3{ static_cast<float>(luaL_checknumber(L, 4)),
                 static_cast<float>(luaL_checknumber(L, 5)),
                 static_cast<float>(luaL_checknumber(L, 6)) },
        static_cast<float>(luaL_optnumber(L, 7, 1000.0)), hits, optional_layer_mask(L, 8));

    lua_newtable(L);
    for (size_t i = 0; i < hits.size(); ++i) {
        push_hit(L, hits[i]);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// world.sphere_cast(x,y,z, dx,dy,dz, radius [, max_distance [, layer_mask]])
int l_world_sphere_cast(lua_State* L) {
    RaycastHit hit;
    const bool did_hit = PhysicsEngine::get_instance().sphere_cast(
        DVector3{ luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3) },
        Vector3{ static_cast<float>(luaL_checknumber(L, 4)),
                 static_cast<float>(luaL_checknumber(L, 5)),
                 static_cast<float>(luaL_checknumber(L, 6)) },
        static_cast<float>(luaL_checknumber(L, 7)),
        static_cast<float>(luaL_optnumber(L, 8, 1000.0)), hit, optional_layer_mask(L, 9));
    if (!did_hit) { lua_pushnil(L); return 1; }
    push_hit(L, hit);
    return 1;
}

// world.overlap_sphere(x,y,z, radius [, layer_mask]) -> array of actor names.
int l_world_overlap_sphere(lua_State* L) {
    std::vector<Actor*> actors;
    PhysicsEngine::get_instance().overlap_sphere(
        DVector3{ luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3) },
        static_cast<float>(luaL_checknumber(L, 4)), actors, optional_layer_mask(L, 5));

    lua_newtable(L);
    int index = 1;
    for (Actor* actor : actors) {
        if (!actor) continue;
        lua_pushstring(L, actor->get_name().c_str());
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

// world.layer_mask("Player", "Enemy", ...) builds a mask from layer names, so a
// script never has to hard-code the numbers the editor assigned.
int l_world_layer_mask(lua_State* L) {
    uint32_t mask = 0;
    const int count = lua_gettop(L);
    for (int argument = 1; argument <= count; ++argument) {
        if (lua_isnumber(L, argument)) {
            const int layer = static_cast<int>(lua_tointeger(L, argument));
            if (layer >= 0 && layer < PhysicsEngine::kLayerCount) mask |= (1u << layer);
            continue;
        }
        const char* wanted = luaL_checkstring(L, argument);
        for (int layer = 0; layer < PhysicsEngine::kLayerCount; ++layer) {
            if (PhysicsEngine::get_layer_name(layer) == wanted) { mask |= (1u << layer); break; }
        }
    }
    lua_pushinteger(L, mask);
    return 1;
}

// --- Character controller --------------------------------------------------

CharacterControllerComponent* character_of(Actor* actor) {
    return actor ? actor->get_component<CharacterControllerComponent>() : nullptr;
}

int l_character_move(lua_State* L) {
    if (auto* c = character_of(checked_actor(L))) c->set_move_input(arg_float(L, 1), arg_float(L, 2));
    return 0;
}
int l_character_jump(lua_State* L) {
    if (auto* c = character_of(checked_actor(L))) c->request_jump();
    return 0;
}
int l_character_is_grounded(lua_State* L) {
    auto* c = character_of(checked_actor(L));
    lua_pushboolean(L, c && c->is_grounded());
    return 1;
}
int l_character_get_speed(lua_State* L) {
    auto* c = character_of(checked_actor(L));
    if (!c) { lua_pushnumber(L, 0.0); return 1; }
    Vector3 v = c->get_velocity();
    lua_pushnumber(L, std::sqrt(v.x * v.x + v.z * v.z));
    return 1;
}

// --- Animation -------------------------------------------------------------

AnimationPlayer* animator_of(Actor* actor) {
    if (!actor) return nullptr;
    auto* mesh = actor->get_component<StaticMeshComponent>();
    return mesh ? mesh->get_animator() : nullptr;
}

int l_anim_play(lua_State* L) {
    auto* animator = animator_of(checked_actor(L));
    if (!animator) { lua_pushboolean(L, 0); return 1; }
    bool loop = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2);
    // Accepts a clip name or a 1-based index, so scripts can address clips either way.
    if (lua_isnumber(L, 1)) {
        animator->play(static_cast<int>(lua_tointeger(L, 1)) - 1, loop);
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, animator->play(std::string(luaL_checkstring(L, 1)), loop));
    }
    return 1;
}
int l_anim_stop(lua_State* L) {
    if (auto* a = animator_of(checked_actor(L))) a->stop();
    return 0;
}
int l_anim_is_playing(lua_State* L) {
    auto* a = animator_of(checked_actor(L));
    lua_pushboolean(L, a && a->is_playing());
    return 1;
}
int l_anim_set_speed(lua_State* L) {
    if (auto* a = animator_of(checked_actor(L))) a->set_speed(arg_float(L, 1));
    return 0;
}
int l_anim_clip_count(lua_State* L) {
    auto* a = animator_of(checked_actor(L));
    lua_pushinteger(L, a ? a->get_clip_count() : 0);
    return 1;
}

// Resolves a clip argument to a 0-based index. Scripts may address a clip either by
// name or by its 1-based position in the asset, matching anim.play().
int anim_clip_arg(lua_State* L, AnimationPlayer* animator, int index) {
    if (lua_isnumber(L, index)) return static_cast<int>(lua_tointeger(L, index)) - 1;
    return animator->find_clip(std::string(luaL_checkstring(L, index)));
}

int l_anim_crossfade(lua_State* L) {
    auto* a = animator_of(checked_actor(L));
    if (!a) { lua_pushboolean(L, 0); return 1; }
    const int clip = anim_clip_arg(L, a, 1);
    // A default of a quarter second: long enough to read as a blend rather than a
    // cut, short enough that a script that omits it still feels responsive.
    const float fade = lua_isnoneornil(L, 2) ? 0.25f : arg_float(L, 2);
    const bool loop = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3);
    if (clip < 0 || clip >= a->get_clip_count()) { lua_pushboolean(L, 0); return 1; }
    a->crossfade(clip, fade, loop);
    lua_pushboolean(L, 1);
    return 1;
}

int l_anim_blend(lua_State* L) {
    auto* a = animator_of(checked_actor(L));
    if (!a) { lua_pushboolean(L, 0); return 1; }
    const int clip = anim_clip_arg(L, a, 1);
    const float weight = arg_float(L, 2);
    const float fade = lua_isnoneornil(L, 3) ? 0.25f : arg_float(L, 3);
    if (clip < 0 || clip >= a->get_clip_count()) { lua_pushboolean(L, 0); return 1; }
    a->blend(clip, weight, fade);
    lua_pushboolean(L, 1);
    return 1;
}

int l_anim_stop_clip(lua_State* L) {
    auto* a = animator_of(checked_actor(L));
    if (!a) { lua_pushboolean(L, 0); return 1; }
    const int clip = anim_clip_arg(L, a, 1);
    if (clip < 0 || clip >= a->get_clip_count()) { lua_pushboolean(L, 0); return 1; }
    a->stop_clip(clip);
    lua_pushboolean(L, 1);
    return 1;
}

int l_anim_set_weight(lua_State* L) {
    auto* a = animator_of(checked_actor(L));
    if (!a) return 0;
    const int clip = anim_clip_arg(L, a, 1);
    if (clip >= 0) a->set_clip_weight(clip, arg_float(L, 2));
    return 0;
}

int l_anim_get_weight(lua_State* L) {
    auto* a = animator_of(checked_actor(L));
    if (!a) { lua_pushnumber(L, 0.0); return 1; }
    const int clip = anim_clip_arg(L, a, 1);
    lua_pushnumber(L, (clip >= 0) ? a->get_clip_weight(clip) : 0.0f);
    return 1;
}

int l_anim_set_layer(lua_State* L) {
    auto* a = animator_of(checked_actor(L));
    if (!a) return 0;
    const int clip = anim_clip_arg(L, a, 1);
    if (clip >= 0) a->set_clip_layer(clip, static_cast<int>(arg_float(L, 2)));
    return 0;
}

int l_anim_set_additive(lua_State* L) {
    auto* a = animator_of(checked_actor(L));
    if (!a) return 0;
    const int clip = anim_clip_arg(L, a, 1);
    const bool additive = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2);
    if (clip >= 0) {
        a->set_clip_blend_mode(clip, additive ? AnimationPlayer::BlendMode::Additive
                                              : AnimationPlayer::BlendMode::Blend);
    }
    return 0;
}

// anim.set_mask(clip, bone_name [, include_descendants]) restricts a clip to a
// sub-tree; anim.set_mask(clip) with no bone clears the mask.
int l_anim_set_mask(lua_State* L) {
    auto* a = animator_of(checked_actor(L));
    if (!a) { lua_pushboolean(L, 0); return 1; }
    const int clip = anim_clip_arg(L, a, 1);
    if (clip < 0 || clip >= a->get_clip_count()) { lua_pushboolean(L, 0); return 1; }
    if (lua_isnoneornil(L, 2)) {
        a->clear_clip_bone_mask(clip);
        lua_pushboolean(L, 1);
        return 1;
    }
    const bool recursive = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3);
    lua_pushboolean(L, a->set_clip_bone_mask(clip, std::string(luaL_checkstring(L, 2)), recursive));
    return 1;
}

int l_anim_add_mask(lua_State* L) {
    auto* a = animator_of(checked_actor(L));
    if (!a) { lua_pushboolean(L, 0); return 1; }
    const int clip = anim_clip_arg(L, a, 1);
    if (clip < 0 || clip >= a->get_clip_count()) { lua_pushboolean(L, 0); return 1; }
    const bool recursive = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3);
    lua_pushboolean(L, a->add_clip_bone_mask(clip, std::string(luaL_checkstring(L, 2)), recursive));
    return 1;
}

int l_anim_clip_name(lua_State* L) {
    auto* a = animator_of(checked_actor(L));
    if (!a) { lua_pushnil(L); return 1; }
    const int clip = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    if (clip < 0 || clip >= a->get_clip_count()) { lua_pushnil(L); return 1; }
    lua_pushstring(L, a->get_clip_name(clip).c_str());
    return 1;
}

int l_anim_get_time(lua_State* L) {
    auto* a = animator_of(checked_actor(L));
    if (!a) { lua_pushnumber(L, 0.0); return 1; }
    if (lua_isnoneornil(L, 1)) { lua_pushnumber(L, a->get_time_seconds()); return 1; }
    const int clip = anim_clip_arg(L, a, 1);
    lua_pushnumber(L, (clip >= 0) ? a->get_clip_time(clip) : 0.0f);
    return 1;
}

int l_anim_get_duration(lua_State* L) {
    auto* a = animator_of(checked_actor(L));
    if (!a) { lua_pushnumber(L, 0.0); return 1; }
    if (lua_isnoneornil(L, 1)) { lua_pushnumber(L, a->get_duration_seconds()); return 1; }
    const int clip = anim_clip_arg(L, a, 1);
    lua_pushnumber(L, (clip >= 0) ? a->get_clip_duration_seconds(clip) : 0.0f);
    return 1;
}

// --- Joints ----------------------------------------------------------------

// The named joint on this actor, or its first joint when no name is given. An
// actor can carry several - a ragdoll bone is constrained to its parent and its
// children - so the name is how a script addresses a particular one.
JointComponent* joint_of(Actor* actor, lua_State* L, int name_index) {
    if (!actor) return nullptr;
    const char* wanted = lua_isnoneornil(L, name_index) ? nullptr : luaL_checkstring(L, name_index);
    for (const auto& comp : actor->get_components()) {
        auto* joint = dynamic_cast<JointComponent*>(comp.get());
        if (!joint) continue;
        if (!wanted || joint->get_name() == wanted) return joint;
    }
    return nullptr;
}

int l_joint_set_motor(lua_State* L) {
    // joint.set_motor(target_velocity [, joint_name])
    auto* joint = joint_of(checked_actor(L), L, 2);
    if (!joint) { lua_pushboolean(L, 0); return 1; }
    joint->set_motor_enabled(true);
    joint->set_motor_target(arg_float(L, 1));
    lua_pushboolean(L, 1);
    return 1;
}

int l_joint_stop_motor(lua_State* L) {
    auto* joint = joint_of(checked_actor(L), L, 1);
    if (!joint) { lua_pushboolean(L, 0); return 1; }
    joint->set_motor_enabled(false);
    lua_pushboolean(L, 1);
    return 1;
}

// Hinge angle in degrees, or slider offset in metres.
int l_joint_value(lua_State* L) {
    auto* joint = joint_of(checked_actor(L), L, 1);
    lua_pushnumber(L, joint ? joint->get_current_value() : 0.0f);
    return 1;
}

int l_joint_exists(lua_State* L) {
    auto* joint = joint_of(checked_actor(L), L, 1);
    lua_pushboolean(L, joint && joint->has_joint());
    return 1;
}

// --- UI --------------------------------------------------------------------

// Widgets are addressed by name across every canvas on the actor, because a script
// thinks in terms of "the health bar", not "the second widget of the first canvas".
UIWidget* widget_of(Actor* actor, const char* widget_name) {
    if (!actor || !widget_name) return nullptr;
    for (const auto& comp : actor->get_components()) {
        auto* canvas = dynamic_cast<UICanvasComponent*>(comp.get());
        if (!canvas) continue;
        if (UIWidget* found = canvas->find(widget_name)) return found;
    }
    return nullptr;
}

UIWidget* checked_widget(lua_State* L, int index) {
    return widget_of(checked_actor(L), luaL_checkstring(L, index));
}

int l_ui_set_text(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    if (w) w->text = luaL_checkstring(L, 2);
    lua_pushboolean(L, w != nullptr);
    return 1;
}

int l_ui_get_text(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    if (!w) { lua_pushnil(L); return 1; }
    lua_pushstring(L, w->text.c_str());
    return 1;
}

int l_ui_set_visible(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    if (w) w->visible = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2);
    lua_pushboolean(L, w != nullptr);
    return 1;
}

int l_ui_is_visible(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    lua_pushboolean(L, w && w->visible);
    return 1;
}

int l_ui_set_interactive(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    if (w) w->interactive = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2);
    lua_pushboolean(L, w != nullptr);
    return 1;
}

int l_ui_set_value(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    if (w) w->value = arg_float(L, 2);
    lua_pushboolean(L, w != nullptr);
    return 1;
}

int l_ui_get_value(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    lua_pushnumber(L, w ? w->value : 0.0f);
    return 1;
}

// The value as a 0..1 fraction of the widget's range, which is what a health bar
// actually wants to be told.
int l_ui_set_fraction(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    if (w) w->set_fraction(arg_float(L, 2));
    lua_pushboolean(L, w != nullptr);
    return 1;
}

int l_ui_get_fraction(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    lua_pushnumber(L, w ? w->fraction() : 0.0f);
    return 1;
}

int l_ui_set_color(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    if (w) {
        w->background_color = { arg_float(L, 2), arg_float(L, 3), arg_float(L, 4),
                                lua_isnoneornil(L, 5) ? 1.0f : arg_float(L, 5) };
    }
    lua_pushboolean(L, w != nullptr);
    return 1;
}

int l_ui_set_text_color(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    if (w) {
        w->text_color = { arg_float(L, 2), arg_float(L, 3), arg_float(L, 4),
                          lua_isnoneornil(L, 5) ? 1.0f : arg_float(L, 5) };
    }
    lua_pushboolean(L, w != nullptr);
    return 1;
}

int l_ui_set_fill_color(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    if (w) {
        w->fill_color = { arg_float(L, 2), arg_float(L, 3), arg_float(L, 4),
                          lua_isnoneornil(L, 5) ? 1.0f : arg_float(L, 5) };
    }
    lua_pushboolean(L, w != nullptr);
    return 1;
}

int l_ui_set_image(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    if (w) w->image_path = luaL_checkstring(L, 2);
    lua_pushboolean(L, w != nullptr);
    return 1;
}

// Moves a widget without disturbing its size, by shifting both offsets together.
int l_ui_set_position(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    if (w) {
        const float x = arg_float(L, 2);
        const float y = arg_float(L, 3);
        const float width  = w->offset_max.x - w->offset_min.x;
        const float height = w->offset_max.y - w->offset_min.y;
        w->offset_min = { x, y };
        w->offset_max = { x + width, y + height };
    }
    lua_pushboolean(L, w != nullptr);
    return 1;
}

int l_ui_set_size(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    if (w) {
        w->offset_max = { w->offset_min.x + arg_float(L, 2), w->offset_min.y + arg_float(L, 3) };
    }
    lua_pushboolean(L, w != nullptr);
    return 1;
}

// Polling alternative to the on_ui_click callback, for a script that would rather
// ask than be told. True only on the frame the click completed.
int l_ui_clicked(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    lua_pushboolean(L, w && w->clicked);
    return 1;
}

int l_ui_hovered(lua_State* L) {
    UIWidget* w = checked_widget(L, 1);
    lua_pushboolean(L, w && w->hovered);
    return 1;
}

int l_ui_exists(lua_State* L) {
    lua_pushboolean(L, checked_widget(L, 1) != nullptr);
    return 1;
}

// Shows or hides every widget of a whole canvas at once - a pause menu is one call,
// not one per element.
int l_ui_set_canvas_visible(lua_State* L) {
    Actor* actor = checked_actor(L);
    if (!actor) { lua_pushboolean(L, 0); return 1; }
    const bool value = lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1);
    bool any = false;
    for (const auto& comp : actor->get_components()) {
        if (auto* canvas = dynamic_cast<UICanvasComponent*>(comp.get())) {
            canvas->visible = value;
            any = true;
        }
    }
    lua_pushboolean(L, any);
    return 1;
}

// --- Navigation ------------------------------------------------------------

NavAgentComponent* agent_of(Actor* actor) {
    return actor ? actor->get_component<NavAgentComponent>() : nullptr;
}

// agent.move_to(x, y, z) - finds a route and starts walking it.
int l_agent_move_to(lua_State* L) {
    auto* agent = agent_of(checked_actor(L));
    if (!agent) { lua_pushboolean(L, 0); return 1; }
    const DVector3 target{ static_cast<double>(arg_float(L, 1)),
                           static_cast<double>(arg_float(L, 2)),
                           static_cast<double>(arg_float(L, 3)) };
    lua_pushboolean(L, agent->set_destination(target));
    return 1;
}

// Walks to wherever another actor currently is. The common case for an AI, and it
// saves every script re-deriving the target's position itself.
int l_agent_follow(lua_State* L) {
    auto* agent = agent_of(checked_actor(L));
    Actor* target = find_actor(luaL_checkstring(L, 1));
    if (!agent || !target) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, agent->set_destination(target->get_actor_transform().position));
    return 1;
}

int l_agent_stop(lua_State* L) {
    if (auto* agent = agent_of(checked_actor(L))) agent->stop();
    return 0;
}

int l_agent_is_moving(lua_State* L) {
    auto* agent = agent_of(checked_actor(L));
    lua_pushboolean(L, agent && agent->is_moving());
    return 1;
}

int l_agent_has_arrived(lua_State* L) {
    auto* agent = agent_of(checked_actor(L));
    lua_pushboolean(L, agent && agent->has_arrived());
    return 1;
}

int l_agent_remaining(lua_State* L) {
    auto* agent = agent_of(checked_actor(L));
    lua_pushnumber(L, agent ? agent->remaining_distance() : 0.0f);
    return 1;
}

int l_agent_set_speed(lua_State* L) {
    if (auto* agent = agent_of(checked_actor(L))) agent->speed = arg_float(L, 1);
    return 0;
}

// nav.sample(x, y, z [, max_distance]) - nearest point on the navmesh, or nil.
int l_nav_sample(lua_State* L) {
    const DVector3 near{ static_cast<double>(arg_float(L, 1)),
                         static_cast<double>(arg_float(L, 2)),
                         static_cast<double>(arg_float(L, 3)) };
    const float max_distance = lua_isnoneornil(L, 4) ? 4.0f : arg_float(L, 4);
    DVector3 result;
    if (!NavMesh::get().sample_position(near, max_distance, result)) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, result.x);
    lua_pushnumber(L, result.y);
    lua_pushnumber(L, result.z);
    return 3;
}

// nav.can_walk(x1,y1,z1, x2,y2,z2) - straight-line walkability, for an AI deciding
// whether it needs a path at all.
int l_nav_can_walk(lua_State* L) {
    const DVector3 from{ static_cast<double>(arg_float(L, 1)),
                         static_cast<double>(arg_float(L, 2)),
                         static_cast<double>(arg_float(L, 3)) };
    const DVector3 to{ static_cast<double>(arg_float(L, 4)),
                       static_cast<double>(arg_float(L, 5)),
                       static_cast<double>(arg_float(L, 6)) };
    lua_pushboolean(L, NavMesh::get().line_of_sight(from, to));
    return 1;
}

// nav.path_length(x1,y1,z1, x2,y2,z2) - how far the walk actually is, which is not
// the straight-line distance and is what a "which target is closest" test needs.
// Negative when there is no route.
int l_nav_path_length(lua_State* L) {
    const DVector3 from{ static_cast<double>(arg_float(L, 1)),
                         static_cast<double>(arg_float(L, 2)),
                         static_cast<double>(arg_float(L, 3)) };
    const DVector3 to{ static_cast<double>(arg_float(L, 4)),
                       static_cast<double>(arg_float(L, 5)),
                       static_cast<double>(arg_float(L, 6)) };
    std::vector<DVector3> points;
    if (!NavMesh::get().find_path(from, to, points) || points.size() < 2) {
        lua_pushnumber(L, -1.0);
        return 1;
    }
    double total = 0.0;
    for (size_t i = 0; i + 1 < points.size(); ++i) total += (points[i + 1] - points[i]).length();
    lua_pushnumber(L, total);
    return 1;
}

int l_nav_is_built(lua_State* L) {
    lua_pushboolean(L, NavMesh::get().is_built());
    return 1;
}

// --- Misc ------------------------------------------------------------------

int l_log(lua_State* L) {
    std::string line;
    int argc = lua_gettop(L);
    for (int i = 1; i <= argc; ++i) {
        if (i > 1) line += " ";
        // luaL_tolstring honours __tostring and converts numbers, so tables and
        // booleans print instead of erroring.
        line += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    Actor* actor = current_actor(L);
    std::cout << "[Lua" << (actor ? " " + actor->get_name() : "") << "] " << line << std::endl;
    return 0;
}

void register_table(lua_State* L, const char* name, const luaL_Reg* functions) {
    lua_newtable(L);
    luaL_setfuncs(L, functions, 0);
    lua_setglobal(L, name);
}

// Adds the traceback to a runtime error, which is the difference between "attempt to
// index a nil value" and knowing which line of which script did it.
int message_handler(lua_State* L) {
    const char* message = lua_tostring(L, 1);
    if (!message) message = "(non-string error)";
    luaL_traceback(L, L, message, 1);
    return 1;
}

bool protected_call(lua_State* L, int nargs, std::string& out_error) {
    int handler_index = lua_gettop(L) - nargs - 1;
    if (lua_pcall(L, nargs, 0, handler_index) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        out_error = err ? err : "unknown error";
        lua_pop(L, 1);
        return false;
    }
    return true;
}

} // namespace

namespace LuaAPI {

lua_State* create_state() {
    lua_State* L = luaL_newstate();
    if (!L) return nullptr;

    // Deliberately not luaL_openlibs: io and os would let a downloaded script read
    // and delete the player's files, and package would let it load native code.
    // string/table/math/coroutine are the parts gameplay actually needs.
    static const luaL_Reg safe_libs[] = {
        { LUA_GNAME,      luaopen_base },
        { LUA_TABLIBNAME, luaopen_table },
        { LUA_STRLIBNAME, luaopen_string },
        { LUA_MATHLIBNAME, luaopen_math },
        { LUA_COLIBNAME,  luaopen_coroutine },
        { nullptr, nullptr }
    };
    for (const luaL_Reg* lib = safe_libs; lib->func; ++lib) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
    // Come in with the base library and would defeat the point of leaving package out.
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");

    static const luaL_Reg actor_fns[] = {
        { "get_position", l_get_position }, { "set_position", l_set_position }, { "translate", l_translate },
        { "get_rotation", l_get_rotation }, { "set_rotation", l_set_rotation }, { "rotate", l_rotate },
        { "get_scale", l_get_scale }, { "set_scale", l_set_scale },
        { "get_name", l_get_name },
        { "set_color", l_set_color }, { "set_metallic", l_set_metallic },
        { "set_roughness", l_set_roughness }, { "set_emissive", l_set_emissive },
        { "set_visible", l_set_visible },
        { "get_velocity", l_get_velocity }, { "set_velocity", l_set_velocity },
        { "set_angular_velocity", l_set_angular_velocity },
        { nullptr, nullptr }
    };
    static const luaL_Reg input_fns[] = {
        { "axis", l_input_axis }, { "held", l_input_held }, { "pressed", l_input_pressed },
        { "released", l_input_released }, { "mouse_delta", l_input_mouse_delta },
        { nullptr, nullptr }
    };
    static const luaL_Reg world_fns[] = {
        { "exists", l_world_exists }, { "find_position", l_world_find_position },
        { "distance_to", l_world_distance_to }, { "actor_count", l_world_actor_count },
        { "camera_position", l_world_camera_position },
        { "spawn", l_world_spawn }, { "destroy", l_world_destroy }, { "raycast", l_world_raycast },
        { "raycast_all", l_world_raycast_all }, { "sphere_cast", l_world_sphere_cast },
        { "overlap_sphere", l_world_overlap_sphere }, { "layer_mask", l_world_layer_mask },
        { nullptr, nullptr }
    };
    static const luaL_Reg character_fns[] = {
        { "move", l_character_move }, { "jump", l_character_jump },
        { "is_grounded", l_character_is_grounded }, { "get_speed", l_character_get_speed },
        { nullptr, nullptr }
    };
    static const luaL_Reg anim_fns[] = {
        { "play", l_anim_play }, { "stop", l_anim_stop }, { "is_playing", l_anim_is_playing },
        { "set_speed", l_anim_set_speed }, { "clip_count", l_anim_clip_count },
        { "clip_name", l_anim_clip_name },
        { "time", l_anim_get_time }, { "duration", l_anim_get_duration },
        // Blending. crossfade is the one a state machine wants; blend/set_weight
        // drive a layer that plays alongside the base animation rather than
        // replacing it.
        { "crossfade", l_anim_crossfade }, { "blend", l_anim_blend },
        { "stop_clip", l_anim_stop_clip },
        { "set_weight", l_anim_set_weight }, { "get_weight", l_anim_get_weight },
        { "set_layer", l_anim_set_layer }, { "set_additive", l_anim_set_additive },
        { "set_mask", l_anim_set_mask }, { "add_mask", l_anim_add_mask },
        { nullptr, nullptr }
    };

    static const luaL_Reg joint_fns[] = {
        { "set_motor", l_joint_set_motor }, { "stop_motor", l_joint_stop_motor },
        { "value", l_joint_value }, { "exists", l_joint_exists },
        { nullptr, nullptr }
    };

    static const luaL_Reg ui_fns[] = {
        { "set_text", l_ui_set_text }, { "get_text", l_ui_get_text },
        { "set_visible", l_ui_set_visible }, { "is_visible", l_ui_is_visible },
        { "set_interactive", l_ui_set_interactive },
        { "set_value", l_ui_set_value }, { "get_value", l_ui_get_value },
        { "set_fraction", l_ui_set_fraction }, { "get_fraction", l_ui_get_fraction },
        { "set_color", l_ui_set_color }, { "set_text_color", l_ui_set_text_color },
        { "set_fill_color", l_ui_set_fill_color },
        { "set_image", l_ui_set_image },
        { "set_position", l_ui_set_position }, { "set_size", l_ui_set_size },
        { "clicked", l_ui_clicked }, { "hovered", l_ui_hovered },
        { "exists", l_ui_exists }, { "set_canvas_visible", l_ui_set_canvas_visible },
        { nullptr, nullptr }
    };

    static const luaL_Reg agent_fns[] = {
        { "move_to", l_agent_move_to }, { "follow", l_agent_follow },
        { "stop", l_agent_stop }, { "is_moving", l_agent_is_moving },
        { "has_arrived", l_agent_has_arrived }, { "remaining", l_agent_remaining },
        { "set_speed", l_agent_set_speed },
        { nullptr, nullptr }
    };
    static const luaL_Reg nav_fns[] = {
        { "sample", l_nav_sample }, { "can_walk", l_nav_can_walk },
        { "path_length", l_nav_path_length }, { "is_built", l_nav_is_built },
        { nullptr, nullptr }
    };

    register_table(L, "actor", actor_fns);
    register_table(L, "input", input_fns);
    register_table(L, "world", world_fns);
    register_table(L, "character", character_fns);
    register_table(L, "animation", anim_fns);
    register_table(L, "joint", joint_fns);
    register_table(L, "ui", ui_fns);
    register_table(L, "agent", agent_fns);
    register_table(L, "nav", nav_fns);

    lua_pushcfunction(L, l_log);
    lua_setglobal(L, "log");

    // Spawn kinds, matching Engine::spawn_actor_by_id.
    lua_newtable(L);
    const char* kinds[] = { "CUBE", "SPHERE", "LIGHT", "PARTICLES", "LIGHT_RAY" };
    for (int i = 0; i < 5; ++i) {
        lua_pushinteger(L, i);
        lua_setfield(L, -2, kinds[i]);
    }
    lua_setglobal(L, "SPAWN");

    return L;
}

void destroy_state(lua_State* state) { if (state) lua_close(state); }

void set_current_actor(lua_State* state, Actor* actor) {
    if (!state) return;
    lua_pushlightuserdata(state, (void*)&kCurrentActorKey);
    lua_pushlightuserdata(state, actor);
    lua_settable(state, LUA_REGISTRYINDEX);
}

bool run_source(lua_State* state, const std::string& source, const std::string& chunk_name,
                std::string& out_error) {
    if (!state) { out_error = "no Lua state"; return false; }

    lua_pushcfunction(state, message_handler);
    if (luaL_loadbuffer(state, source.c_str(), source.size(), chunk_name.c_str()) != LUA_OK) {
        const char* err = lua_tostring(state, -1);
        out_error = err ? err : "syntax error";
        lua_pop(state, 2); // error + handler
        return false;
    }
    if (!protected_call(state, 0, out_error)) { lua_pop(state, 1); return false; }
    lua_pop(state, 1); // handler
    return true;
}

bool call_function(lua_State* state, const char* function_name, float delta_time,
                   std::string& out_error) {
    if (!state) return true;

    lua_pushcfunction(state, message_handler);
    lua_getglobal(state, function_name);
    if (!lua_isfunction(state, -1)) {
        // A script defining only one of the two entry points is normal.
        lua_pop(state, 2);
        return true;
    }
    lua_pushnumber(state, delta_time);
    if (!protected_call(state, 1, out_error)) { lua_pop(state, 1); return false; }
    lua_pop(state, 1);
    return true;
}

bool has_function(lua_State* state, const char* function_name) {
    if (!state) return false;
    lua_getglobal(state, function_name);
    bool found = lua_isfunction(state, -1);
    lua_pop(state, 1);
    return found;
}

bool call_collision(lua_State* state, const char* function_name, const char* other_name,
                    const CollisionInfo& info, std::string& out_error) {
    if (!state) return true;

    lua_pushcfunction(state, message_handler);
    lua_getglobal(state, function_name);
    if (!lua_isfunction(state, -1)) { lua_pop(state, 2); return true; }

    if (other_name) lua_pushstring(state, other_name); else lua_pushnil(state);
    lua_pushnumber(state, info.point.x);
    lua_pushnumber(state, info.point.y);
    lua_pushnumber(state, info.point.z);
    lua_pushnumber(state, info.normal.x);
    lua_pushnumber(state, info.normal.y);
    lua_pushnumber(state, info.normal.z);
    lua_pushnumber(state, info.approach_speed);

    if (!protected_call(state, 8, out_error)) { lua_pop(state, 1); return false; }
    lua_pop(state, 1);
    return true;
}

bool call_trigger(lua_State* state, const char* function_name, const char* other_name,
                  std::string& out_error) {
    if (!state) return true;

    lua_pushcfunction(state, message_handler);
    lua_getglobal(state, function_name);
    if (!lua_isfunction(state, -1)) { lua_pop(state, 2); return true; }

    if (other_name) lua_pushstring(state, other_name); else lua_pushnil(state);
    if (!protected_call(state, 1, out_error)) { lua_pop(state, 1); return false; }
    lua_pop(state, 1);
    return true;
}

bool call_ui(lua_State* state, const char* function_name, const char* widget_name,
             bool has_value, float value, std::string& out_error) {
    if (!state) return true;

    lua_pushcfunction(state, message_handler);
    lua_getglobal(state, function_name);
    if (!lua_isfunction(state, -1)) { lua_pop(state, 2); return true; }

    if (widget_name) lua_pushstring(state, widget_name); else lua_pushnil(state);
    int argc = 1;
    if (has_value) { lua_pushnumber(state, value); argc = 2; }
    if (!protected_call(state, argc, out_error)) { lua_pop(state, 1); return false; }
    lua_pop(state, 1);
    return true;
}

bool read_properties(lua_State* state, std::vector<ScriptProperty>& out_properties) {
    out_properties.clear();
    if (!state) return false;

    lua_getglobal(state, "properties");
    if (!lua_istable(state, -1)) { lua_pop(state, 1); return false; }

    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        // Key at -2, value at -1. Only string keys become inspector entries; an
        // array-style table is a script's own data, not a set of named settings.
        if (lua_type(state, -2) == LUA_TSTRING) {
            ScriptProperty property;
            property.name = lua_tostring(state, -2);

            const int value_type = lua_type(state, -1);
            if (value_type == LUA_TNUMBER) {
                property.type = ScriptProperty::Type::Number;
                property.number_value = lua_tonumber(state, -1);
                out_properties.push_back(property);
            } else if (value_type == LUA_TSTRING) {
                property.type = ScriptProperty::Type::String;
                property.string_value = lua_tostring(state, -1);
                out_properties.push_back(property);
            } else if (value_type == LUA_TBOOLEAN) {
                property.type = ScriptProperty::Type::Boolean;
                property.boolean_value = lua_toboolean(state, -1) != 0;
                out_properties.push_back(property);
            }
        }
        // Pop the value, leave the key for the next iteration.
        lua_pop(state, 1);
    }
    lua_pop(state, 1); // the properties table

    // Stable order, so the Inspector does not reshuffle its own rows every reload -
    // Lua's table iteration order is not defined and does change.
    std::sort(out_properties.begin(), out_properties.end(),
              [](const ScriptProperty& a, const ScriptProperty& b) { return a.name < b.name; });
    return true;
}

void write_property(lua_State* state, const ScriptProperty& property) {
    if (!state || property.name.empty()) return;

    lua_getglobal(state, "properties");
    if (!lua_istable(state, -1)) { lua_pop(state, 1); return; }

    switch (property.type) {
        case ScriptProperty::Type::Number:  lua_pushnumber(state, property.number_value); break;
        case ScriptProperty::Type::String:  lua_pushstring(state, property.string_value.c_str()); break;
        case ScriptProperty::Type::Boolean: lua_pushboolean(state, property.boolean_value ? 1 : 0); break;
    }
    lua_setfield(state, -2, property.name.c_str());
    lua_pop(state, 1);
}

bool check_syntax(const std::string& source, const std::string& chunk_name, std::string& out_error) {
    lua_State* L = luaL_newstate();
    if (!L) { out_error = "could not create Lua state"; return false; }
    bool ok = luaL_loadbuffer(L, source.c_str(), source.size(), chunk_name.c_str()) == LUA_OK;
    if (!ok) {
        const char* err = lua_tostring(L, -1);
        out_error = err ? err : "syntax error";
    }
    lua_close(L);
    return ok;
}

} // namespace LuaAPI
