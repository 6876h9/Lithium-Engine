#include "world/lua_script_component.hpp"
#include "world/actor.hpp"
#include "scripting/lua_api.hpp"
#include "world/collision_event.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

LuaScriptComponent::LuaScriptComponent(Actor* owner, const std::string& name, const std::string& script_path)
    : ActorComponent(owner, name), script_path(script_path) {
    if (!script_path.empty()) reload();
}

LuaScriptComponent::~LuaScriptComponent() {
    LuaAPI::destroy_state(state);
}

void LuaScriptComponent::report(const std::string& stage, const std::string& message) {
    last_error = message;
    errored = true;
    std::cerr << "[Lua] " << (owner ? owner->get_name() : "?") << " (" << script_path << ") "
              << stage << ": " << message << std::endl;
}

void LuaScriptComponent::reload() {
    // A fresh VM per reload rather than re-running into the old one: leftover globals
    // from the previous version of the file would otherwise survive and a renamed
    // function would keep being called.
    LuaAPI::destroy_state(state);
    state = nullptr;
    last_error.clear();
    errored = false;

    if (script_path.empty()) return;

    std::ifstream file(script_path);
    if (!file.is_open()) {
        report("load", "could not open file");
        return;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();

    std::error_code ec;
    last_modified_time = std::filesystem::last_write_time(script_path, ec);

    state = LuaAPI::create_state();
    if (!state) {
        report("init", "could not create Lua state");
        return;
    }
    LuaAPI::set_current_actor(state, owner);

    std::string error;
    if (!LuaAPI::run_source(state, buffer.str(), std::filesystem::path(script_path).filename().string(), error)) {
        report("compile", error);
        return;
    }

    // What the script says it can be configured with, and then whatever this actor
    // was configured to. Read before the overrides are applied so the declared
    // values are the script's own defaults, not the previous instance's settings.
    LuaAPI::read_properties(state, declared_properties);
    apply_property_overrides();

    std::cout << "[Lua] Loaded " << script_path << std::endl;
    // If the reload happened mid-session the new VM has not run begin_play yet.
    if (playing) begin_play_pending = true;
}

void LuaScriptComponent::begin_play() {
    playing = true;
    // Reload on Play so the run always uses what is currently on disk, and so a
    // second Play does not inherit the first run's globals.
    reload();
    begin_play_pending = true;
}

void LuaScriptComponent::tick(float delta_time) {
    // Hot reload. Checked before the error latch so fixing a broken script in the
    // editor recovers without having to restart Play.
    if (!script_path.empty()) {
        std::error_code ec;
        auto current = std::filesystem::last_write_time(script_path, ec);
        if (!ec && current != last_modified_time) {
            reload();
        }
    }

    if (errored || !state) return;

    LuaAPI::set_current_actor(state, owner);

    std::string error;
    if (begin_play_pending) {
        begin_play_pending = false;
        if (!LuaAPI::call_function(state, "on_begin_play", 0.0f, error)) {
            report("on_begin_play", error);
            return;
        }
    }

    if (!LuaAPI::call_function(state, "on_tick", delta_time, error)) {
        report("on_tick", error);
    }
}

void LuaScriptComponent::forward_collision(const char* function_name, const CollisionInfo& info) {
    if (errored || !state) return;
    // Checked first so an actor whose script has no collision handler does not pay
    // for a VM entry on every contact, every frame.
    if (!LuaAPI::has_function(state, function_name)) return;

    LuaAPI::set_current_actor(state, owner);
    std::string error;
    if (!LuaAPI::call_collision(state, function_name,
                                info.other ? info.other->get_name().c_str() : nullptr,
                                info, error)) {
        report(function_name, error);
    }
}

void LuaScriptComponent::forward_trigger(const char* function_name, Actor* other) {
    if (errored || !state) return;
    if (!LuaAPI::has_function(state, function_name)) return;

    LuaAPI::set_current_actor(state, owner);
    std::string error;
    if (!LuaAPI::call_trigger(state, function_name, other ? other->get_name().c_str() : nullptr, error)) {
        report(function_name, error);
    }
}

void LuaScriptComponent::on_collision_enter(const CollisionInfo& info) { forward_collision("on_collision_enter", info); }
void LuaScriptComponent::on_collision_stay(const CollisionInfo& info)  { forward_collision("on_collision_stay", info); }
void LuaScriptComponent::on_collision_exit(const CollisionInfo& info)  { forward_collision("on_collision_exit", info); }

void LuaScriptComponent::apply_property_overrides() {
    if (!state) return;

    // Overrides for properties the script no longer declares are left alone rather
    // than written: a script that dropped a setting should not have a stray global
    // pushed back into it, and keeping the value means renaming it back restores it.
    for (const LuaAPI::ScriptProperty& override_value : property_overrides) {
        for (const LuaAPI::ScriptProperty& declared : declared_properties) {
            if (declared.name != override_value.name) continue;
            // The type has to still match. A script that changed a setting from a
            // number to a string has redefined it, and the old value is meaningless.
            if (declared.type == override_value.type) LuaAPI::write_property(state, override_value);
            break;
        }
    }
}

void LuaScriptComponent::forward_ui(const char* function_name, const std::string& widget_name,
                                    bool has_value, float value) {
    if (errored || !state) return;
    if (!LuaAPI::has_function(state, function_name)) return;

    LuaAPI::set_current_actor(state, owner);
    std::string error;
    if (!LuaAPI::call_ui(state, function_name, widget_name.c_str(), has_value, value, error)) {
        report(function_name, error);
    }
}

void LuaScriptComponent::on_ui_click(const std::string& widget_name) {
    forward_ui("on_ui_click", widget_name, false, 0.0f);
}
void LuaScriptComponent::on_ui_value_changed(const std::string& widget_name, float value) {
    forward_ui("on_ui_value_changed", widget_name, true, value);
}

void LuaScriptComponent::on_trigger_enter(Actor* other) { forward_trigger("on_trigger_enter", other); }
void LuaScriptComponent::on_trigger_stay(Actor* other)  { forward_trigger("on_trigger_stay", other); }
void LuaScriptComponent::on_trigger_exit(Actor* other)  { forward_trigger("on_trigger_exit", other); }
