#pragma once

// Thin platform abstraction.
//
// The engine is otherwise portable C++ on portable libraries (SDL2, OpenGL, assimp,
// Jolt, miniaudio). These are the handful of places that genuinely differ between
// Linux and Windows, kept in one header so platform #ifdefs do not spread through
// engine code.

#include <string>
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

} // namespace Platform
