#include "core/platform.hpp"
#if !LITHIUM_PLATFORM_WINDOWS
#include <unistd.h>
#include <sys/wait.h>
#endif
#if LITHIUM_PLATFORM_WINDOWS
#include <windows.h>
#include <shellapi.h>
#endif


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


// --- External processes ----------------------------------------------------

bool executable_exists(const std::string& name) {
    if (name.empty()) return false;

    // An absolute or relative path is checked directly rather than searched for.
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        std::error_code ec;
        return std::filesystem::exists(name, ec);
    }

#if LITHIUM_PLATFORM_WINDOWS
    // "where" searches PATH and PATHEXT, so it finds code.cmd as well as code.exe -
    // which matters, because VS Code ships its CLI as a .cmd shim.
    const std::string command = "where " + name + " >nul 2>&1";
    return std::system(command.c_str()) == 0;
#else
    // Walk PATH directly instead of shelling out to `which`: this runs once per
    // candidate editor when the settings window opens, and a dozen fork/exec pairs
    // to answer a question readdir can answer is a visible stall on a slow machine.
    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;

    const std::string path_list(path_env);
    size_t start = 0;
    while (start <= path_list.size()) {
        const size_t colon = path_list.find(':', start);
        const std::string dir = path_list.substr(
            start, (colon == std::string::npos) ? std::string::npos : colon - start);
        if (!dir.empty()) {
            const std::filesystem::path candidate = std::filesystem::path(dir) / name;
            // access() rather than exists(): a file on PATH that is not executable
            // is not a program, and reporting it as one produces a launch that
            // fails with a permission error the user cannot act on.
            if (::access(candidate.c_str(), X_OK) == 0) return true;
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return false;
#endif
}

bool launch_detached(const std::string& executable, const std::vector<std::string>& arguments) {
    if (executable.empty()) return false;

#if LITHIUM_PLATFORM_WINDOWS
    // CreateProcess takes one command line, so the arguments are joined here. Each
    // is quoted, and embedded quotes escaped, because the child's own C runtime
    // will split this string again and a path with a space in it would otherwise
    // arrive as two arguments.
    std::string command_line = "\"" + executable + "\"";
    for (const std::string& argument : arguments) {
        command_line += " \"";
        for (char c : argument) {
            if (c == '"') command_line += '\\';
            command_line += c;
        }
        command_line += "\"";
    }

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // DETACHED_PROCESS so the editor does not inherit the engine's console and does
    // not die when the engine exits.
    const BOOL ok = CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                                   DETACHED_PROCESS, nullptr, nullptr, &startup, &process);
    if (!ok) return false;
    // The handles are the parent's, not the child's lifetime - closing them here is
    // what makes this fire-and-forget rather than a handle leak per launch.
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return true;
#else
    // argv for execvp: program name first, arguments after, null terminated.
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) return false;

    if (pid == 0) {
        // Double fork. Without it the editor stays a child of the engine, and
        // nothing ever reaps it - the engine has no SIGCHLD handler, so every
        // editor the user closes would leave a zombie for the rest of the session.
        // The intermediate exits immediately, so the editor is reparented to init,
        // which does reap it.
        const pid_t grandchild = ::fork();
        if (grandchild == 0) {
            // Detach from the engine's session so the editor is not killed by a
            // signal sent to the engine's process group (Ctrl+C in its terminal).
            ::setsid();
            ::execvp(executable.c_str(), argv.data());
            // Only reached if exec failed. _exit, not exit: this is a forked copy of
            // the engine, and running its atexit handlers would flush the parent's
            // buffers a second time and tear down GL state the parent still owns.
            ::_exit(127);
        }
        ::_exit(grandchild < 0 ? 127 : 0);
    }

    // Reap the intermediate, which has already exited. This is the only wait, and it
    // returns immediately - the editor itself is never waited on.
    int status = 0;
    ::waitpid(pid, &status, 0);
    return true;
#endif
}

bool open_with_default_application(const std::string& path) {
    if (path.empty()) return false;
#if LITHIUM_PLATFORM_WINDOWS
    const HINSTANCE result = ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecute returns a value above 32 on success; the codes at or below that
    // are its error enum, not a handle.
    return reinterpret_cast<INT_PTR>(result) > 32;
#else
    return launch_detached("xdg-open", { path });
#endif
}

} // namespace Platform
