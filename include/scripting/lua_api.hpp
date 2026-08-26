#pragma once

#include <string>
#include <vector>

struct lua_State;
struct CollisionInfo;
class Actor;

// The engine's Lua binding layer.
//
// Each script component owns its own lua_State rather than sharing one global VM.
// That costs a little memory per scripted actor but means a script cannot clobber
// another's globals, a runtime error takes down only its own actor, and - since
// actor ticks run across a task graph - two scripts never touch the same VM
// concurrently, which a shared state would require locking to guarantee.
namespace LuaAPI {

// One value a script has declared as configurable.
//
// A script publishes these by defining a global table called `properties`:
//
//     properties = { speed = 5.0, target = "Player", can_fly = false }
//
// The engine reads it after the chunk runs, shows each entry in the Inspector, and
// writes whatever the designer set back into the same table before on_begin_play.
// The script then just reads `properties.speed` - it never has to know the value
// came from outside.
//
// Three types, because these are what an inspector can meaningfully edit and what a
// gameplay script actually tunes. A reference to another actor is expressed as its
// name, which is how the rest of the scripting API already addresses actors.
struct ScriptProperty {
    enum class Type { Number, String, Boolean };

    std::string name;
    Type type = Type::Number;
    double number_value = 0.0;
    std::string string_value;
    bool boolean_value = false;
};

// Reads the script's `properties` table. Entries of any other type are skipped
// rather than reported: a script is free to keep a nested table in there for its own
// use, it simply will not appear in the Inspector.
bool read_properties(lua_State* state, std::vector<ScriptProperty>& out_properties);

// Writes one value into the script's `properties` table. Does nothing if the script
// has no such table, which is the normal case for a script that declares none.
void write_property(lua_State* state, const ScriptProperty& property);

// Creates a VM with the safe parts of the standard library and the engine API
// already installed. Returns null if the VM could not be created.
lua_State* create_state();
void destroy_state(lua_State* state);

// Points the API at the actor whose script this is. Called before every entry into
// the script, because the same VM is reused across frames.
void set_current_actor(lua_State* state, Actor* actor);

// Runs a chunk of source. Returns false and fills out_error on a syntax or runtime
// error; the VM stays usable either way.
bool run_source(lua_State* state, const std::string& source, const std::string& chunk_name,
                std::string& out_error);

// Calls a global function taking (dt) if it exists. Missing is not an error - a
// script with only on_begin_play is perfectly valid.
bool call_function(lua_State* state, const char* function_name, float delta_time,
                   std::string& out_error);

// Calls a collision handler taking (other_name, px, py, pz, nx, ny, nz, speed).
// other_name is nil when the other actor no longer exists.
bool call_collision(lua_State* state, const char* function_name, const char* other_name,
                    const struct CollisionInfo& info, std::string& out_error);

// Calls a trigger handler taking (other_name). Triggers have no contact geometry.
bool call_trigger(lua_State* state, const char* function_name, const char* other_name,
                  std::string& out_error);

// Calls a UI handler taking (widget_name) and, when has_value is set, the widget's
// current value as a second argument.
bool call_ui(lua_State* state, const char* function_name, const char* widget_name,
             bool has_value, float value, std::string& out_error);

// Returns true if the script defines a global function by this name, so the engine
// can skip the call entirely for the common case of a script with no handlers.
bool has_function(lua_State* state, const char* function_name);

// Syntax-checks without running, for the editor's "Check for errors" button.
bool check_syntax(const std::string& source, const std::string& chunk_name, std::string& out_error);

} // namespace LuaAPI
