#include "core/input_map.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

namespace {
// Below this, stick drift reads as real input and a character walks on its own.
constexpr float kAxisDeadzone = 0.20f;
// An analogue action counts as "held" past this, so pressed()/released() work the
// same whether an action is bound to a key or to a trigger.
constexpr float kHeldThreshold = 0.50f;
}

InputMap& InputMap::get() {
    static InputMap instance;
    return instance;
}

InputMap::~InputMap() {
    if (pad_) {
        SDL_GameControllerClose(pad_);
        pad_ = nullptr;
    }
}

void InputMap::load_defaults() {
    actions_.clear();

    auto add = [this](const std::string& name, std::vector<InputBinding> b) {
        InputAction a;
        a.name = name;
        a.bindings = std::move(b);
        actions_.push_back(std::move(a));
    };

    // Paired opposite bindings on one action rather than separate Forward/Back
    // actions: gameplay code reads a single signed value and a stick maps onto it
    // directly.
    add("MoveForward", {
        { InputSource::Key, SDL_SCANCODE_W,  1.0f },
        { InputSource::Key, SDL_SCANCODE_S, -1.0f },
        { InputSource::PadAxis, SDL_CONTROLLER_AXIS_LEFTY, -1.0f },
    });
    add("MoveRight", {
        { InputSource::Key, SDL_SCANCODE_D,  1.0f },
        { InputSource::Key, SDL_SCANCODE_A, -1.0f },
        { InputSource::PadAxis, SDL_CONTROLLER_AXIS_LEFTX, 1.0f },
    });
    add("Jump", {
        { InputSource::Key, SDL_SCANCODE_SPACE, 1.0f },
        { InputSource::PadButton, SDL_CONTROLLER_BUTTON_A, 1.0f },
    });
    add("Fire", {
        { InputSource::MouseButton, SDL_BUTTON_LEFT, 1.0f },
        { InputSource::PadAxis, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1.0f },
    });
    add("AltFire", {
        { InputSource::MouseButton, SDL_BUTTON_RIGHT, 1.0f },
        { InputSource::PadAxis, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1.0f },
    });
    add("Sprint", {
        { InputSource::Key, SDL_SCANCODE_LSHIFT, 1.0f },
        { InputSource::PadButton, SDL_CONTROLLER_BUTTON_LEFTSTICK, 1.0f },
    });
    add("Crouch", {
        { InputSource::Key, SDL_SCANCODE_LCTRL, 1.0f },
        { InputSource::PadButton, SDL_CONTROLLER_BUTTON_B, 1.0f },
    });
    add("Interact", {
        { InputSource::Key, SDL_SCANCODE_E, 1.0f },
        { InputSource::PadButton, SDL_CONTROLLER_BUTTON_X, 1.0f },
    });
}

void InputMap::refresh_gamepad() {
    if (pad_ && SDL_GameControllerGetAttached(pad_)) return;
    if (pad_) {
        SDL_GameControllerClose(pad_);
        pad_ = nullptr;
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            pad_ = SDL_GameControllerOpen(i);
            if (pad_) {
                std::cout << "[Input] Gamepad connected: " << gamepad_name() << std::endl;
                break;
            }
        }
    }
}

const char* InputMap::gamepad_name() const {
    if (!pad_) return "None";
    const char* n = SDL_GameControllerName(pad_);
    return n ? n : "Unknown";
}

void InputMap::update() {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    Uint32 mouse = SDL_GetMouseState(nullptr, nullptr);

    int rx = 0, ry = 0;
    SDL_GetRelativeMouseState(&rx, &ry);
    mouse_dx_ = static_cast<float>(rx);
    mouse_dy_ = static_cast<float>(ry);

    for (auto& a : actions_) {
        a.prev_held = a.held;

        // Bindings accumulate rather than override, so W and S on one action cancel
        // out when both are down instead of the last one evaluated winning.
        float value = 0.0f;
        for (const auto& b : a.bindings) {
            float raw = 0.0f;
            switch (b.source) {
                case InputSource::Key:
                    if (keys && b.code >= 0 && b.code < SDL_NUM_SCANCODES) {
                        raw = keys[b.code] ? 1.0f : 0.0f;
                    }
                    break;
                case InputSource::MouseButton:
                    raw = (mouse & SDL_BUTTON(b.code)) ? 1.0f : 0.0f;
                    break;
                case InputSource::PadButton:
                    if (pad_) {
                        raw = SDL_GameControllerGetButton(
                                  pad_, static_cast<SDL_GameControllerButton>(b.code)) ? 1.0f : 0.0f;
                    }
                    break;
                case InputSource::PadAxis:
                    if (pad_) {
                        float v = SDL_GameControllerGetAxis(
                                      pad_, static_cast<SDL_GameControllerAxis>(b.code)) / 32767.0f;
                        if (std::fabs(v) < kAxisDeadzone) v = 0.0f;
                        raw = v;
                    }
                    break;
            }
            value += raw * b.scale;
        }

        a.value = std::max(-1.0f, std::min(1.0f, value));
        a.held = std::fabs(a.value) >= kHeldThreshold;
    }
}

int InputMap::index_of(const std::string& name) const {
    for (size_t i = 0; i < actions_.size(); ++i) {
        if (actions_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

InputAction* InputMap::action_at(int i) {
    if (i < 0 || i >= static_cast<int>(actions_.size())) return nullptr;
    return &actions_[i];
}

const InputAction* InputMap::action_at(int i) const {
    if (i < 0 || i >= static_cast<int>(actions_.size())) return nullptr;
    return &actions_[i];
}

int InputMap::add_action(const std::string& name) {
    // Names are the lookup key from gameplay code, so duplicates would make one of
    // the two permanently unreachable.
    std::string unique = name;
    for (int n = 1; index_of(unique) != -1 && n < 1000; ++n) {
        unique = name + "_" + std::to_string(n);
    }
    InputAction a;
    a.name = unique;
    actions_.push_back(std::move(a));
    return static_cast<int>(actions_.size()) - 1;
}

void InputMap::remove_action(int i) {
    if (i < 0 || i >= static_cast<int>(actions_.size())) return;
    actions_.erase(actions_.begin() + i);
}

bool InputMap::held(int i) const     { const auto* a = action_at(i); return a && a->held; }
bool InputMap::pressed(int i) const  { const auto* a = action_at(i); return a && a->held && !a->prev_held; }
bool InputMap::released(int i) const { const auto* a = action_at(i); return a && !a->held && a->prev_held; }
float InputMap::axis(int i) const    { const auto* a = action_at(i); return a ? a->value : 0.0f; }

bool InputMap::held(const std::string& n) const     { return held(index_of(n)); }
bool InputMap::pressed(const std::string& n) const  { return pressed(index_of(n)); }
bool InputMap::released(const std::string& n) const { return released(index_of(n)); }
float InputMap::axis(const std::string& n) const    { return axis(index_of(n)); }

std::string InputMap::describe(const InputBinding& b) {
    std::string label;
    switch (b.source) {
        case InputSource::Key: {
            const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(b.code));
            label = (n && *n) ? n : "Key " + std::to_string(b.code);
            break;
        }
        case InputSource::MouseButton:
            label = "Mouse " + std::to_string(b.code);
            break;
        case InputSource::PadButton: {
            const char* n = SDL_GameControllerGetStringForButton(
                                static_cast<SDL_GameControllerButton>(b.code));
            label = std::string("Pad ") + ((n && *n) ? n : std::to_string(b.code));
            break;
        }
        case InputSource::PadAxis: {
            const char* n = SDL_GameControllerGetStringForAxis(
                                static_cast<SDL_GameControllerAxis>(b.code));
            label = std::string("Axis ") + ((n && *n) ? n : std::to_string(b.code));
            break;
        }
    }
    if (b.scale < 0.0f) label += " (-)";
    return label;
}

bool InputMap::save(const std::string& path) const {
    json j;
    j["version"] = 1;
    j["actions"] = json::array();
    for (const auto& a : actions_) {
        json aj;
        aj["name"] = a.name;
        aj["bindings"] = json::array();
        for (const auto& b : a.bindings) {
            aj["bindings"].push_back({
                {"source", static_cast<int>(b.source)},
                {"code", b.code},
                {"scale", b.scale}
            });
        }
        j["actions"].push_back(aj);
    }

    std::ofstream f(path);
    if (!f) {
        std::cerr << "[Input] Could not write " << path << std::endl;
        return false;
    }
    f << j.dump(4);
    return true;
}

bool InputMap::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;

    json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        std::cerr << "[Input] Failed to parse " << path << ": " << e.what() << std::endl;
        return false;
    }
    if (!j.contains("actions")) return false;

    actions_.clear();
    for (const auto& aj : j["actions"]) {
        InputAction a;
        a.name = aj.value("name", std::string("Action"));
        if (aj.contains("bindings")) {
            for (const auto& bj : aj["bindings"]) {
                InputBinding b;
                b.source = static_cast<InputSource>(bj.value("source", 0));
                b.code = bj.value("code", 0);
                b.scale = bj.value("scale", 1.0f);
                a.bindings.push_back(b);
            }
        }
        actions_.push_back(std::move(a));
    }
    return true;
}
