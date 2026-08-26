#pragma once

#include "world/component.hpp"
#include "scripting/lua_api.hpp"
#include <vector>
#include <filesystem>
#include <string>

struct lua_State;

// Runs a Lua gameplay script attached to an actor.
//
// The script defines free functions the engine calls:
//   function on_begin_play()  end
//   function on_tick(dt)      end
// and reaches the engine through the actor / input / world / character / animation
// tables installed by LuaAPI.
class LuaScriptComponent : public ActorComponent {
public:
    LuaScriptComponent(Actor* owner, const std::string& name, const std::string& script_path = "");
    virtual ~LuaScriptComponent();

    virtual void begin_play() override;
    virtual void tick(float delta_time) override;

    // Forwarded to the script's on_collision_* / on_trigger_* globals. Absent
    // handlers cost nothing - the call is skipped rather than entering the VM.
    virtual void on_collision_enter(const CollisionInfo& info) override;
    virtual void on_collision_stay(const CollisionInfo& info) override;
    virtual void on_collision_exit(const CollisionInfo& info) override;
    virtual void on_trigger_enter(Actor* other) override;
    virtual void on_trigger_stay(Actor* other) override;
    virtual void on_trigger_exit(Actor* other) override;

    // Forwarded to the script's on_ui_click / on_ui_value_changed globals.
    virtual void on_ui_click(const std::string& widget_name) override;
    virtual void on_ui_value_changed(const std::string& widget_name, float value) override;

    // Rebuilds the VM and re-runs the file. Called on attach, on Play, and whenever
    // the file changes on disk.
    void reload();

    std::string script_path;

    // --- Inspector properties ------------------------------------------------
    // What the script declared in its `properties` table, with its defaults. Rebuilt
    // on every reload, because the script is the authority on what exists.
    const std::vector<LuaAPI::ScriptProperty>& get_declared_properties() const {
        return declared_properties;
    }
    // What this actor overrode. Serialised with the scene, and written back into the
    // VM after every reload so a hot reload does not discard the designer's values.
    std::vector<LuaAPI::ScriptProperty> property_overrides;
    // Pushes property_overrides into the running VM. Call after editing one.
    void apply_property_overrides();
    const std::string& get_last_error() const { return last_error; }
    bool has_error() const { return errored; }

private:
    void report(const std::string& stage, const std::string& message);
    void forward_collision(const char* function_name, const CollisionInfo& info);
    void forward_trigger(const char* function_name, Actor* other);
    void forward_ui(const char* function_name, const std::string& widget_name, bool has_value, float value);

    std::vector<LuaAPI::ScriptProperty> declared_properties;

    lua_State* state = nullptr;
    std::filesystem::file_time_type last_modified_time{};
    std::string last_error;
    // Latched on failure so a script erroring every frame does not flood the log or
    // burn time re-entering a VM that is already broken. Cleared by a reload, which
    // is what makes fixing the file in the editor pick straight back up.
    bool errored = false;
    // begin_play only fires once per Play session, but a hot reload mid-session has
    // to run it again - the fresh VM has none of the previous one's state.
    bool begin_play_pending = false;
    bool playing = false;
};
