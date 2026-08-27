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

    // Makes the script's entry points callable.
    //
    // A precompiled module under kModuleDir is preferred over invoking the
    // compiler. That is what an exported game ships: requiring a player to have
    // g++ installed to run a game is not a distribution story, so the export step
    // builds every script ahead of time and the runtime here just loads the result.
    // Falling back to compiling keeps the editor's edit-and-reload workflow, where
    // a toolchain genuinely is present.
    bool compile_and_load();

    // Builds `script_path` into `out_module_path`. Static so the exporter can
    // precompile a project's scripts without constructing components for them.
    // Returns false and fills out_log on any failure.
    static bool compile_script(const std::string& script_path,
                               const std::string& out_module_path,
                               std::string& out_log);

    // Module filename a given script compiles to, without a directory. Export and
    // load both route through this so they cannot disagree about the name.
    static std::string module_name_for(const std::string& script_path);

    // Where an exported game keeps its precompiled script modules, relative to the
    // executable.
    static const char* kModuleDir;

private:
    void unload();
    bool load_module(const std::string& module_path);

    void* dl_handle = nullptr;
    void (*on_begin_play_ptr)(Actor*) = nullptr;
    void (*on_tick_ptr)(Actor*, float) = nullptr;
};
