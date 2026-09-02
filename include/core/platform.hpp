#pragma once

// Thin platform abstraction.
//
// The engine is otherwise portable C++ on portable libraries (SDL2, OpenGL, assimp,
// Jolt, miniaudio). These are the handful of places that genuinely differ between
// Linux and Windows, kept in one header so platform #ifdefs do not spread through
// engine code.

#include <string>
#include <vector>
#include <filesystem>

#if defined(_WIN32)
    #define LITHIUM_PLATFORM_WINDOWS 1
    #define LITHIUM_PLATFORM_LINUX   0
#else
    #define LITHIUM_PLATFORM_WINDOWS 0
    #define LITHIUM_PLATFORM_LINUX   1
#endif

namespace Platform {

// Absolute path of the running executable.
// Linux reads /proc/self/exe; Windows uses GetModuleFileNameW. Needed because the
// engine anchors its working directory to itself so relative asset paths resolve no
// matter where it was launched from.
std::filesystem::path executable_path();

// Directory containing the running executable.
std::filesystem::path executable_dir();

// --- Dynamic library loading (native C++ scripting) ------------------------
// dlopen/dlsym/dlclose on Linux, LoadLibrary/GetProcAddress/FreeLibrary on Windows.
using DynamicLibrary = void*;

DynamicLibrary library_open(const std::string& path);
void*          library_symbol(DynamicLibrary handle, const std::string& name);
void           library_close(DynamicLibrary handle);
std::string    library_last_error();

// Platform file extension for a loadable module: ".so" or ".dll".
const char* shared_library_extension();

// --- Native script compilation --------------------------------------------
// Compiling C++ at runtime needs a toolchain on the *user's* machine. On Linux that
// is normally g++; on Windows it rarely exists, so callers must handle absence.
bool        has_cpp_compiler();
std::string cpp_compiler_command();

// Directory for short-lived build artefacts (/tmp, or %TEMP% on Windows).
std::filesystem::path temp_dir();

// --- External processes ----------------------------------------------------
// True if `name` is runnable - found on PATH, or an absolute path to an existing
// executable. Used to detect which code editors are actually installed rather than
// offering the user a list of things that will fail to launch.
bool executable_exists(const std::string& name);

// Launches a program and returns immediately, without waiting for it to exit.
//
// This is the whole point: std::system blocks until the child finishes, so opening
// a file in an editor that way would freeze the engine for as long as the editor
// stayed open. Uses fork/exec on Linux and CreateProcess on Windows, and the child
// is detached so closing the engine does not take the editor with it.
//
// `arguments` is passed through verbatim - no shell is involved, so a path with
// spaces or quotes in it needs no escaping and cannot be reinterpreted as a command.
bool launch_detached(const std::string& executable, const std::vector<std::string>& arguments);

// Opens a path with whatever the desktop has associated with it (xdg-open, or
// ShellExecute on Windows). The fallback when no specific editor is configured.
bool open_with_default_application(const std::string& path);

} // namespace Platform
