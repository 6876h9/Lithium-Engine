# CMake toolchain for cross-compiling Lithium Engine to Windows x86-64 from Linux
# using MinGW-w64.
#
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
#   cmake --build build-win -j$(nproc)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

# The -posix variants specifically, not the bare names.
#
# MinGW-w64 ships two threading models. The win32 one has no std::thread,
# std::mutex or std::condition_variable in libstdc++, and this engine uses all
# three (the resource manager's thread pool, the task graph, the asset database).
# Building against it fails deep inside <bits/std_mutex.h> with '__gthread_cond_t
# does not name a type', which reads like a broken standard library rather than a
# toolchain selection.
#
# The unsuffixed x86_64-w64-mingw32-g++ follows update-alternatives, so which model
# you get depends on the machine's configuration rather than on anything in this
# repository. Naming the -posix binaries makes the cross-build reproducible.
find_program(MINGW_C_COMPILER   NAMES ${TOOLCHAIN_PREFIX}-gcc-posix ${TOOLCHAIN_PREFIX}-gcc)
find_program(MINGW_CXX_COMPILER NAMES ${TOOLCHAIN_PREFIX}-g++-posix ${TOOLCHAIN_PREFIX}-g++)

if(NOT MINGW_C_COMPILER OR NOT MINGW_CXX_COMPILER)
    message(FATAL_ERROR
        "MinGW-w64 cross-compiler not found. Install it with:\n"
        "  sudo apt install g++-mingw-w64-x86-64")
endif()

set(CMAKE_C_COMPILER   ${MINGW_C_COMPILER})
set(CMAKE_CXX_COMPILER ${MINGW_CXX_COMPILER})
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})

# Look for programs on the host, but headers and libraries only in the target root -
# otherwise the build silently picks up Linux .so files and fails at link time.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static libgcc/libstdc++ so the .exe does not need MinGW runtime DLLs alongside it.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
