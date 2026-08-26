#pragma once

#include <SDL2/SDL.h>
#include <string>
#include <vector>

// Named input actions with rebindable bindings.
//
// Game code asks "is Jump held?" rather than "is SDLK_SPACE down?", which is what
// makes a game rebindable at all: the binding lives in data the player can change,
// not in a scancode compiled into a script.
enum class InputSource : int {
    Key = 0,          // SDL_Scancode
    MouseButton = 1,  // SDL_BUTTON_*
    PadButton = 2,    // SDL_GameControllerButton
    PadAxis = 3       // SDL_GameControllerAxis
};

struct InputBinding {
    InputSource source = InputSource::Key;
    int code = 0;
    // Applied to this binding's contribution. A -1 scale is what lets one axis action
    // ("MoveForward") carry both W and S, rather than needing a second action for the
    // opposite direction.
    float scale = 1.0f;
};

struct InputAction {
    std::string name;
    std::vector<InputBinding> bindings;

    // Per-frame state, refreshed by InputMap::update().
    bool held = false;
    bool prev_held = false;
    float value = 0.0f;   // analogue result, -1..1 for axis-style actions
};

class InputMap {
public:
    static InputMap& get();

    // The set a new project starts with, so movement and camera work out of the box.
    void load_defaults();
    bool load(const std::string& path);
    bool save(const std::string& path) const;

    // Samples every binding. Called once per frame from the engine, before scripts
    // run, so a script sees a consistent snapshot for the whole frame.
    void update();

    // Opens the first attached controller, if any. Safe to call repeatedly.
    void refresh_gamepad();
    bool has_gamepad() const { return pad_ != nullptr; }
    const char* gamepad_name() const;

    int action_count() const { return static_cast<int>(actions_.size()); }
    int index_of(const std::string& name) const;
    InputAction* action_at(int i);
    const InputAction* action_at(int i) const;

    int add_action(const std::string& name);
    void remove_action(int i);

    // By name, for C++ gameplay code.
    bool held(const std::string& name) const;
    bool pressed(const std::string& name) const;
    bool released(const std::string& name) const;
    float axis(const std::string& name) const;

    // By index, for the C-minus VM - its call arguments are floats, so it has no way
    // to pass an action name through.
    bool held(int i) const;
    bool pressed(int i) const;
    bool released(int i) const;
    float axis(int i) const;

    // Human-readable binding label for the rebinding UI ("Space", "Mouse 1", "Pad A").
    static std::string describe(const InputBinding& b);

    float mouse_dx() const { return mouse_dx_; }
    float mouse_dy() const { return mouse_dy_; }

private:
    InputMap() = default;
    ~InputMap();
    InputMap(const InputMap&) = delete;
    InputMap& operator=(const InputMap&) = delete;

    std::vector<InputAction> actions_;
    SDL_GameController* pad_ = nullptr;
    float mouse_dx_ = 0.0f;
    float mouse_dy_ = 0.0f;
};
