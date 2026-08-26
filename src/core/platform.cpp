#include "core/platform.hpp"

#include <cstdlib>
#include <vector>

#if LITHIUM_PLATFORM_WINDOWS
    #include <windows.h>
#else
    #include <dlfcn.h>
    #include <unistd.h>
#endif

namespace Platform {

std::filesystem::path executable_path() {
#if LITHIUM_PLATFORM_WINDOWS
    // MAX_PATH is not a real limit on modern Windows, so grow until it fits rather
    // than silently truncating a long install path.
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0) return {};
        if (len < buffer.size() - 1) return std::filesystem::path(buffer.data());
        buffer.resize(buffer.size() * 2);
    }
#else
    std::error_code ec;
    std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return p;
#endif
}

std::filesystem::path executable_dir() {
    std::filesystem::path exe = executable_path();
    return exe.empty() ? std::filesystem::path{} : exe.parent_path();
}

DynamicLibrary library_open(const std::string& path) {
#if LITHIUM_PLATFORM_WINDOWS
    return reinterpret_cast<DynamicLibrary>(LoadLibraryA(path.c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* library_symbol(DynamicLibrary handle, const std::string& name) {
    if (!handle) return nullptr;
#if LITHIUM_PLATFORM_WINDOWS
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name.c_str()));
#else
    return dlsym(handle, name.c_str());
#endif
}

void library_close(DynamicLibrary handle) {
    if (!handle) return;
#if LITHIUM_PLATFORM_WINDOWS
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

std::string library_last_error() {
#if LITHIUM_PLATFORM_WINDOWS
    DWORD code = GetLastError();
    if (code == 0) return "no error";
    LPSTR text = nullptr;
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&text, 0, nullptr);
    std::string msg = (len && text) ? std::string(text, len) : ("error code " + std::to_string(code));
    if (text) LocalFree(text);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    return msg;
#else
    const char* e = dlerror();
    return e ? std::string(e) : std::string("no error");
#endif
}

const char* shared_library_extension() {
#if LITHIUM_PLATFORM_WINDOWS
    return ".dll";
#else
    return ".so";
#endif
}

bool has_cpp_compiler() {
#if LITHIUM_PLATFORM_WINDOWS
    // Windows has no system compiler by default. Probe for a MinGW g++ on PATH;
    // "where" is the Windows equivalent of "which".
    return std::system("where g++ >nul 2>&1") == 0;
#else
    return std::system("which g++ > /dev/null 2>&1") == 0;
#endif
}

std::string cpp_compiler_command() {
    return "g++";
}

std::filesystem::path temp_dir() {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::temp_directory_path(ec);
    if (ec || p.empty()) {
#if LITHIUM_PLATFORM_WINDOWS
        return std::filesystem::path(".");
#else
        return std::filesystem::path("/tmp");
#endif
    }
    return p;
}

} // namespace Platform
