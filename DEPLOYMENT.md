# Lithium Engine — Linux Deployment Guide

Two distribution paths, both self-contained and both runnable from any folder:

1. **Portable folder + `launch_engine.sh`** — a zip the user extracts anywhere.
2. **`.AppImage`** — one executable file, no extraction, no installation.

---

## Portability fixes this required

Two defects made the previous build non-distributable. Both are fixed; they are
documented here because they will silently come back if the build setup changes.

**1. Absolute `RUNPATH`.** The binary recorded

```
RUNPATH = /home/<builder>/rtc/custom_engine/thirdparty/embree
```

so Embree resolved only on the build machine. `CMakeLists.txt` now sets

```cmake
INSTALL_RPATH "$ORIGIN/lib:$ORIGIN"
```

`$ORIGIN` is expanded by the dynamic loader relative to the executable, so bundled
libraries are found wherever the folder is unpacked. Verify with:

```bash
readelf -d build/Lithium_Engine | grep RUNPATH     # must show $ORIGIN, never /home/...
```

**2. Assets resolved against the working directory.** Every asset path
(`Content/...`, `EngineContent/...`) is relative, so launching from anywhere other
than the install folder found nothing. `main.cpp` now anchors the working directory
to the executable at startup, resolving any file arguments to absolute paths first so
a relative path typed on the command line still means what the shell meant.

---

## 1. Portable folder

Layout to ship:

```
LithiumEngine/
├── launch_engine.sh        <- entry point
├── Lithium_Engine
├── lib/                    <- libembree4, libsycl, libtbb, libur_loader
├── EngineContent/          <- DefaultSky.hdr, NightSky.hdr, Skies/
└── Content/                <- meshes, textures
```

Build and stage:

```bash
cmake --build build --target Lithium_Engine Lithium_Engine_content -j$(nproc)
```

The content target copies assets **and** the bundled libraries into `build/lib`.

### What `launch_engine.sh` does

- Resolves its own directory (following symlinks), so the folder can live anywhere.
- Runs `ldd` on the binary and collects anything reported `not found`. Asking the
  loader beats a hand-written list: it is exact, covers transitive dependencies, and
  stays correct when the engine's dependencies change.
- Checks for an OpenGL driver separately — SDL loads it at runtime, so it never
  appears in `ldd`, and a missing driver is the most common failure on a fresh install.
- Splits what is missing into two categories:
  - **system libraries** → maps them to package names and offers to install;
  - **libraries the engine ships** → these cannot be installed from a repository, so
    it reports the archive as incomplete instead of sending the user after a package
    that does not exist.
- Detects `apt-get`, `pacman`, `dnf`, `yum`, `zypper`, or `apk`, prints the exact
  command, and offers to run it. Only prompts when attached to a terminal.
- Re-checks after installing and refuses to launch if anything is still missing,
  rather than replacing a clear message with a loader crash.

Run it:

```bash
chmod +x launch_engine.sh
./launch_engine.sh
```

---

## 2. AppImage

```bash
./packaging/build_appimage.sh              # defaults to ./build
```

Produces `Lithium_Engine-x86_64.AppImage`. Requires
[`linuxdeploy`](https://github.com/linuxdeploy/linuxdeploy/releases) and
[`appimagetool`](https://github.com/AppImage/appimagetool/releases); if either is
missing the script stops with the AppDir prepared and prints the command to finish.

### Layout

```
AppDir/
├── AppRun                          <- entry point, uses $APPDIR
├── lithium-engine.desktop
├── .DirIcon -> lithium-engine.svg
└── usr/
    ├── bin/     Lithium_Engine, Content/, EngineContent/, lib -> ../lib/lithium
    ├── lib/     host libs bundled by linuxdeploy
    └── lib/lithium/   Embree + SYCL/TBB runtime
```

`usr/bin/lib` is a symlink so the binary's `$ORIGIN/lib` RUNPATH resolves both inside
the packed image and when running the AppDir directly during testing.

### What must NOT be bundled

`linuxdeploy` excludes these by default; do not override it:

| Library | Why |
| --- | --- |
| `libGL`, `libGLX`, `libEGL`, `libGLdispatch` | Must match the host's kernel driver. A bundled copy cannot reach it and breaks hardware acceleration. |
| `libc`, `libm`, `libpthread`, `libdl` | A glibc newer than the host's fails to load; older breaks everything linked against it. |
| `libX11`, `libxcb`, `libwayland-*` | Must match the running display server. |
| `libdrm`, `libgbm` | Driver-coupled. |

Embree, SYCL and TBB **are** bundled: they are not available from distro
repositories in the versions this links against.

### Note on size

The image is ~314 MB, almost entirely the 200-HDRI sky library
(`EngineContent/Skies`, ~301 MB). If that is too large for itch.io, ship a handful of
skies in the build and offer the rest as a separate download — the picker scans the
directory at runtime, so users can drop extra `.hdr` files in with no rebuild.

---

## 3. Publishing to itch.io

Mark the upload **"executable — Linux"**. With
[butler](https://itch.io/docs/butler/):

```bash
# Portable folder
butler push LithiumEngine/ yourname/lithium-engine:linux

# Or the single-file AppImage
butler push Lithium_Engine-x86_64.AppImage yourname/lithium-engine:linux-appimage
```

The archive stores Unix permissions and command-line `unzip` restores them, but some
graphical/cross-platform extractors discard the executable bit. butler always
preserves it; otherwise tell users to run
`chmod +x Lithium_Engine launch_engine.sh` if they hit "Permission denied".

---

## 4. Verifying a build is portable

```bash
readelf -d build/Lithium_Engine | grep RUNPATH        # $ORIGIN only, never /home/...
ldd build/Lithium_Engine | grep -c "not found"        # 0
```

Do **not** check this with `ldd ... | grep /home`. `$ORIGIN` expands to wherever the
binary currently sits, so inside the build tree the bundled libraries legitimately
resolve to `/home/<you>/.../build/lib`. That is correct behaviour, not a baked path.
The way to tell the difference is to move the binary and confirm the paths follow it:

```bash
mkdir -p /tmp/portable && cp -r build/{Lithium_Engine,lib,EngineContent,Content} /tmp/portable/
ldd /tmp/portable/Lithium_Engine | grep embree      # must now say /tmp/portable/lib/...
cd / && /tmp/portable/Lithium_Engine                 # must start and find its assets
```

That last check is the one that matters — it is what caught both defects above.
