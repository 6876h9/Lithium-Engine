#include "world/cpp_script_component.hpp"
#include <iostream>
#include "core/platform.hpp"
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <array>

CppScriptComponent::CppScriptComponent(Actor* owner, const std::string& name, const std::string& script_path)
    : ActorComponent(owner, name), script_path(script_path) {
    if (!script_path.empty()) {
        compile_and_load();
    }
}

CppScriptComponent::~CppScriptComponent() {
    unload();
}

void CppScriptComponent::unload() {
    on_begin_play_ptr = nullptr;
    on_tick_ptr = nullptr;
    if (dl_handle) {
        Platform::library_close(dl_handle);
        dl_handle = nullptr;
    }
}

bool CppScriptComponent::compile_and_load() {
    unload();
    has_error = false;
    build_log = "";

    if (script_path.empty() || !std::filesystem::exists(script_path)) {
        has_error = true;
        build_log = "Script path is empty or file does not exist.\n";
        return false;
    }

    // Native C++ scripting compiles the script on the user's machine at runtime, so a
    // toolchain has to be present. That is normal on Linux and unusual on Windows, so
    // check first and fail with an explanation rather than an opaque compiler error.
    if (!Platform::has_cpp_compiler()) {
        has_error = true;
#if LITHIUM_PLATFORM_WINDOWS
        build_log = "No C++ compiler found on PATH.\n"
                    "Native C++ scripting compiles scripts at runtime and needs g++ available.\n"
                    "Install MSYS2/MinGW-w64 and add its bin directory to PATH, or use\n"
                    "C-Minus / V-Script instead - those are interpreted and need no toolchain.\n";
#else
        build_log = "No C++ compiler found. Install g++ (build-essential) to use native C++ scripting.\n";
#endif
        return false;
    }

    std::filesystem::path module_path = Platform::temp_dir() /
        (std::filesystem::path(script_path).filename().string() + "_" +
         std::to_string(reinterpret_cast<uintptr_t>(this)) + Platform::shared_library_extension());
    std::string out_so = module_path.string();

    // -fPIC is a no-op warning on Windows (all code is position independent there),
    // so it is only passed on Linux.
#if LITHIUM_PLATFORM_WINDOWS
    const char* platform_flags = "-shared -std=c++20";
#else
    const char* platform_flags = "-shared -fPIC -std=c++20";
#endif
    std::string compile_cmd = Platform::cpp_compiler_command() + " " + platform_flags +
        " -I./include -I../include -I./src -I../src \"" + script_path + "\" -o \"" + out_so + "\" 2>&1";

    // popen/pclose are _popen/_pclose in the MSVC-style CRT.
#if LITHIUM_PLATFORM_WINDOWS
    FILE* pipe = _popen(compile_cmd.c_str(), "r");
#else
    FILE* pipe = popen(compile_cmd.c_str(), "r");
#endif
    if (!pipe) {
        has_error = true;
        build_log = "Failed to run the C++ compiler.\n";
        return false;
    }

    std::array<char, 256> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        build_log += buffer.data();
    }

#if LITHIUM_PLATFORM_WINDOWS
    int result = _pclose(pipe);
#else
    int result = pclose(pipe);
#endif
    if (result != 0) {
        has_error = true;
        build_log += "\nCompilation failed with exit code " + std::to_string(result) + ".\n";
        return false;
    }

    // Load the shared library
    dl_handle = Platform::library_open(out_so);
    if (!dl_handle) {
        has_error = true;
        build_log = std::string("Failed to load shared library: ") + Platform::library_last_error().c_str() + "\n";
        return false;
    }

    // Load functions (they are optional, so we don't error out if one is missing, but we log it)
    on_begin_play_ptr = reinterpret_cast<void (*)(Actor*)>(Platform::library_symbol(dl_handle, "on_begin_play"));
    on_tick_ptr = reinterpret_cast<void (*)(Actor*, float)>(Platform::library_symbol(dl_handle, "on_tick"));

    build_log += "Successfully compiled and loaded " + out_so + "\n";
    if (!on_begin_play_ptr && !on_tick_ptr) {
        build_log += "Warning: Neither 'on_begin_play' nor 'on_tick' were found in the script.\n";
        build_log += "Ensure you are wrapping them in extern \"C\".\n";
    }

    return true;
}

void CppScriptComponent::begin_play() {
    if (on_begin_play_ptr) {
        on_begin_play_ptr(owner);
    }
}

void CppScriptComponent::tick(float delta_time) {
    if (on_tick_ptr) {
        on_tick_ptr(owner, delta_time);
    }
}
