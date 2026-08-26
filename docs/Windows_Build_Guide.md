# Building Lithium Engine for Windows

Cross-compiled from Linux with **MinGW-w64**. No Windows machine is needed to build —
only to run the result.

---

## 1. Toolchain

```bash
sudo apt-get install -y mingw-w64        # Debian / Ubuntu
sudo pacman -S mingw-w64-gcc             # Arch
sudo dnf install mingw64-gcc-c++         # Fedora
```

Check:

```bash
x86_64-w64-mingw32-g++ --version
```

## 2. Windows dependencies

Two prebuilt packages live under `thirdparty/`, already fetched:

| Path | Contents | Source |
| --- | --- | --- |
| `thirdparty/SDL2_windows/` | `SDL2.dll`, import libs, headers | SDL 2.30.9 MinGW dev package |
| `thirdparty/embree_windows/` | `embree4.dll`, `tbb12.dll`, `embree4.lib`, headers | Embree 4.4.1 x64 Windows |

The Embree version deliberately matches the Linux build (4.4.1). The **non-SYCL**
package is used, because the engine only calls Embree's CPU C API (`rtcNewDevice`,
`rtcIntersect1`, …) — which also means Windows drops the SYCL, TBB-loader and
`ur_loader` baggage the Linux build carries.

Everything else — assimp, Jolt, enet, zlib — is built from source by FetchContent and
cross-compiles as part of the build.

## 3. Build

```bash
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
cmake --build build-win -j$(nproc)
```

Output in `build-win/`: `Lithium_Engine.exe`, `Lithium_Game.exe`, the required DLLs
beside them, and `Content/` + `EngineContent/`.

The toolchain file links `-static-libgcc -static-libstdc++`, so the executables do not
need MinGW runtime DLLs shipped alongside.

## 4. What differs from the Linux build

| Concern | Linux | Windows |
| --- | --- | --- |
| Library search | `RUNPATH=$ORIGIN/lib`, libs in `lib/` | DLLs beside the `.exe` (Windows has no RPATH) |
| Executable path | `/proc/self/exe` | `GetModuleFileNameW` |
| Dynamic loading | `dlopen`/`dlsym` | `LoadLibrary`/`GetProcAddress` |
| Mesa GL override | `MESA_GL_VERSION_OVERRIDE=4.5` | Not set — vendor drivers report their real version |
| Embree runtime | embree4 + sycl + tbb + ur_loader | embree4 + tbb12 + tbbmalloc |
| Platform libs | `dl`, `pthread`, `m` | `ws2_32`, `winmm`, `imm32`, `setupapi`, … |

All of this sits behind `include/core/platform.hpp` rather than `#ifdef`s scattered
through engine code.

## 5. Native C++ scripting on Windows

C-Minus and V-Script are interpreted and work identically on both platforms.

**Native C++ scripting compiles the script on the user's machine at runtime**, so it
needs `g++` on `PATH`. That is normal on Linux and rare on Windows. The engine now
checks first and explains what to install rather than failing with an opaque compiler
error. Users who want it need MSYS2/MinGW-w64 installed and on `PATH`.

## 6. Packaging

```
LithiumEngine-Windows/
├── Lithium_Engine.exe
├── SDL2.dll  embree4.dll  tbb12.dll  tbbmalloc.dll
├── EngineContent/
└── Content/
```

No launcher script is needed. The dependency-scanning `launch_engine.sh` exists
because Linux distributions vary in what is installed; on Windows every required DLL
ships in the folder, and the only host requirement is a GPU driver with OpenGL 4.5.

Ship it as a zip and mark the itch.io upload **"executable — Windows"**.

## 7. Known risks with this route

Stated plainly, because a build succeeding is not the same as a build working:

- **MinGW linking against an MSVC-built Embree.** Embree exposes a C API, so this
  normally works, but it is the least conventional part of this setup. If the linker
  rejects `embree4.lib`, generate a MinGW import library from the DLL instead:
  ```bash
  cd thirdparty/embree_windows/bin
  gendef embree4.dll
  x86_64-w64-mingw32-dlltool -d embree4.def -l libembree4.a
  ```
  then point `EMBREE_LIBRARIES` at `libembree4.a`.
- **Cross-compiled binaries are not runtime-tested on Linux.** A clean cross-build
  proves it compiles and links, not that it runs. Test on a real Windows machine
  before shipping.
- **MSVC would be the more conventional choice** for a Windows game — better debugging
  and driver-vendor tooling. The CMake now supports either; only the toolchain file
  is MinGW-specific.
