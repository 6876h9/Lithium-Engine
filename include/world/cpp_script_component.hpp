#pragma once

#include "world/component.hpp"
#include <string>

class CppScriptComponent : public ActorComponent {
public:
    CppScriptComponent(Actor* owner, const std::string& name, const std::string& script_path = "");
    ~CppScriptComponent();

    void begin_play() override;
    void tick(float delta_time) override;

    std::string script_path;
    std::string build_log;
    bool has_error = false;
    
    // Compile the script into a shared library and load it
    bool compile_and_load();

private:
    void unload();

    void* dl_handle = nullptr;
    void (*on_begin_play_ptr)(Actor*) = nullptr;
    void (*on_tick_ptr)(Actor*, float) = nullptr;
};
