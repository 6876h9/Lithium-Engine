#include "world/cpp_script_component.hpp"
#include <iostream>
#include "core/platform.hpp"
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <array>

const char* CppScriptComponent::kModuleDir = "Scripts";

namespace {

// Include roots handed to the compiler, in the order they are tried.
//
// "include" is what an exported game has: the export step copies the engine
// headers next to the runtime. "../include" is what the editor has, because it
// runs from the build directory. Passing a root that does not exist is harmless,
// so both are always offered rather than probed - which keeps one command line
// working in both layouts.
const char* kIncludeRoots[] = { "include", "../include" };

// Quotes a path for the shell and rejects the characters that would end the quote
// early. Script paths reach here from a project file, so a path containing a quote
// or a backtick must not be able to extend the compiler invocation into a second
// command.
bool quote_for_shell(const std::string& path, std::string& out) {
    for (char c : path) {
        if (c == '"' || c == '\'' || c == '`' || c == '$' || c == '\\' || c == '\n' || c == '\r') {
            return false;
        }
    }
    out = "\"" + path + "\"";
    return true;
}

} // namespace

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

std::string CppScriptComponent::module_name_for(const std::string& script_path) {
    // Stem plus the platform's library extension. The stem alone is enough because
    // a project's scripts are addressed by name in the editor, and two scripts with
    // the same filename in different folders would already be ambiguous there.
    std::filesystem::path p(script_path);
    return p.stem().string() + Platform::shared_library_extension();
}

bool CppScriptComponent::compile_script(const std::string& script_path,
                                        const std::string& out_module_path,
                                        std::string& out_log) {
    out_log.clear();

    if (script_path.empty() || !std::filesystem::exists(script_path)) {
        out_log = "Script path is empty or file does not exist.\n";
        return false;
    }

    // Native C++ scripting compiles the script on the machine doing the building, so
    // a toolchain has to be present. That is normal on Linux and unusual on Windows,
    // so check first and fail with an explanation rather than an opaque error.
    if (!Platform::has_cpp_compiler()) {
#if LITHIUM_PLATFORM_WINDOWS
        out_log = "No C++ compiler found on PATH.\n"
                  "Native C++ scripting is compiled ahead of time and needs g++ available.\n"
                  "Install MSYS2/MinGW-w64 and add its bin directory to PATH, or use\n"
                  "C-Minus / V-Script instead - those are interpreted and need no toolchain.\n";
#else
        out_log = "No C++ compiler found. Install g++ (build-essential) to use native C++ scripting.\n";
#endif
        return false;
    }

    std::string quoted_script;
    std::string quoted_out;
    if (!quote_for_shell(script_path, quoted_script) || !quote_for_shell(out_module_path, quoted_out)) {
        out_log = "Script or output path contains characters that cannot be passed to the "
                  "compiler safely (quote, backslash, backtick, $ or a newline). Rename it.\n";
        return false;
    }

    // -fPIC is a no-op warning on Windows (all code is position independent there),
    // so it is only passed on Linux.
#if LITHIUM_PLATFORM_WINDOWS
    const char* platform_flags = "-shared -std=c++20";
#else
    const char* platform_flags = "-shared -fPIC -std=c++20";
#endif

    std::string includes;
    for (const char* root : kIncludeRoots) {
        std::string quoted_root;
        if (quote_for_shell(root, quoted_root)) includes += " -I" + quoted_root;
    }

    const std::string compile_cmd = Platform::cpp_compiler_command() + " " + platform_flags +
        includes + " " + quoted_script + " -o " + quoted_out + " 2>&1";

    // popen/pclose are _popen/_pclose in the MSVC-style CRT.
#if LITHIUM_PLATFORM_WINDOWS
    FILE* pipe = _popen(compile_cmd.c_str(), "r");
#else
    FILE* pipe = popen(compile_cmd.c_str(), "r");
#endif
    if (!pipe) {
        out_log = "Failed to run the C++ compiler.\n";
        return false;
    }

    std::array<char, 256> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        out_log += buffer.data();
    }

#if LITHIUM_PLATFORM_WINDOWS
    int result = _pclose(pipe);
#else
    int result = pclose(pipe);
#endif
    if (result != 0) {
        out_log += "\nCompilation failed with exit code " + std::to_string(result) + ".\n";
        return false;
    }
    return true;
}

bool CppScriptComponent::load_module(const std::string& module_path) {
    dl_handle = Platform::library_open(module_path);
    if (!dl_handle) {
        has_error = true;
        build_log += std::string("Failed to load shared library: ") +
                     Platform::library_last_error().c_str() + "\n";
        return false;
    }

    // Both entry points are optional - a script that only ticks is legitimate - so a
    // missing one is not an error. Neither being present almost always means the
    // extern "C" wrapper was forgotten, which is worth saying explicitly because the
    // symbols are otherwise mangled and silently unfindable.
    on_begin_play_ptr = reinterpret_cast<void (*)(Actor*)>(Platform::library_symbol(dl_handle, "on_begin_play"));
    on_tick_ptr = reinterpret_cast<void (*)(Actor*, float)>(Platform::library_symbol(dl_handle, "on_tick"));

    build_log += "Loaded " + module_path + "\n";
    if (!on_begin_play_ptr && !on_tick_ptr) {
        build_log += "Warning: Neither 'on_begin_play' nor 'on_tick' were found in the script.\n";
        build_log += "Ensure you are wrapping them in extern \"C\".\n";
    }
    return true;
}

bool CppScriptComponent::compile_and_load() {
    unload();
    has_error = false;
    build_log = "";

    if (script_path.empty()) {
        has_error = true;
        build_log = "Script path is empty.\n";
        return false;
    }

    // An exported game ships its scripts already built, so it never needs a
    // compiler. This is checked first for that reason - and only accepted when the
    // module is at least as new as the source, so that in the editor an edited
    // script is rebuilt rather than masked by a stale module from a previous export.
    const std::filesystem::path precompiled =
        std::filesystem::path(kModuleDir) / module_name_for(script_path);
    std::error_code ec;
    if (std::filesystem::exists(precompiled, ec) && !ec) {
        bool usable = true;
        if (std::filesystem::exists(script_path, ec) && !ec) {
            const auto module_time = std::filesystem::last_write_time(precompiled, ec);
            if (!ec) {
                const auto source_time = std::filesystem::last_write_time(script_path, ec);
                if (!ec && source_time > module_time) usable = false;
            }
        }
        if (usable && load_module(precompiled.string())) return true;
        // A precompiled module that failed to load is reported, then the compiler is
        // tried, so a corrupt or mismatched module does not strand a script that
        // could still be built from source.
        has_error = false;
        unload();
    }

    if (!std::filesystem::exists(script_path)) {
        has_error = true;
        build_log += "Script file does not exist: " + script_path + "\n";
        return false;
    }

    const std::filesystem::path module_path = Platform::temp_dir() /
        (std::filesystem::path(script_path).filename().string() + "_" +
         std::to_string(reinterpret_cast<uintptr_t>(this)) + Platform::shared_library_extension());

    std::string log;
    if (!compile_script(script_path, module_path.string(), log)) {
        has_error = true;
        build_log += log;
        return false;
    }
    build_log += log;

    return load_module(module_path.string());
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
