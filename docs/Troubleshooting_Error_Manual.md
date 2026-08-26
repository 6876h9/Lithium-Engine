# Lithium Engine — Troubleshooting & Error Manual

**Start here.** Almost every failure is explained in the engine's own log:

```bash
cat LithiumEngine_Startup.log
```

It sits next to the executable and is rewritten each launch. All engine output goes
there, not to the terminal — so a silent crash is not a silent failure.

---

## 1. Missing `.so` dependency matrix

Symptom:

```
./Lithium_Engine: error while loading shared libraries: libNAME.so.N:
cannot open shared object file: No such file or directory
```

First: run `./launch_engine.sh` instead. It identifies the missing library, maps it to
your distribution's package, and offers to install it. The table below is for manual
resolution.

| Missing library | Debian / Ubuntu | Arch | Fedora | openSUSE |
| --- | --- | --- | --- | --- |
| `libGL.so.1`, `libGLX.so.0`, `libOpenGL.so.0`, `libGLdispatch.so.0` | `libgl1 libglx-mesa0 libglvnd0` | `libglvnd mesa` | `libglvnd-glx mesa-libGL` | `Mesa-libGL1 libglvnd` |
| `libSDL2-2.0.so.0` | `libsdl2-2.0-0` | `sdl2` | `SDL2` | `libSDL2-2_0-0` |
| `libX11.so.6` | `libx11-6` | `libx11` | `libX11` | `libX11-6` |
| `libX11-xcb.so.1`, `libxcb*.so` | `libx11-xcb1 libxcb1` | `libxcb` | `libxcb` | `libxcb1` |
| `libwayland-client.so.0`, `libwayland-egl.so.1` | `libwayland-client0 libwayland-egl1` | `wayland` | `wayland-libs` | `libwayland-client0` |
| `libdecor-0.so.0` | `libdecor-0-0` | `libdecor` | `libdecor` | `libdecor-0-0` |
| `libasound.so.2` | `libasound2` | `alsa-lib` | `alsa-lib` | `alsa` |
| `libpulse.so.0` | `libpulse0` | `libpulse` | `pulseaudio-libs` | `libpulse0` |
| `libstdc++.so.6` | `libstdc++6` | `gcc-libs` | `libstdc++` | `libstdc++6` |
| `libgcc_s.so.1` | `libgcc-s1` | `gcc-libs` | `libgcc` | `libgcc_s1` |
| `libdrm.so.2` | `libdrm2` | `libdrm` | `libdrm` | `libdrm2` |
| `libgbm.so.1` | `libgbm1` | `mesa` | `mesa-libgbm` | `libgbm1` |

### These four are different — do not install them

`libembree4.so.4` · `libsycl.so.8` · `libtbb.so.12` · `libur_loader.so.0`

They **ship with the engine** in `lib/`. No distribution packages the versions this
links against. If one is reported missing, the archive is incomplete or `lib/` was
moved or renamed:

```bash
ls lib/          # all four must be present, next to the binary
```

Re-extract the full archive keeping its folder structure. The launcher detects this
case specifically and says so rather than sending you after a nonexistent package.

### Diagnosing manually

```bash
ldd Lithium_Engine | grep "not found"
```

---

## 2. Graphics failures

### No OpenGL 4.5 / context creation fails

```bash
glxinfo | grep "OpenGL core profile version"
```

Below 4.5 the engine will not start. Install your GPU vendor's driver, or
`mesa-utils` + `libgl1-mesa-dri` for open-source drivers.

### "COMPUTE SHADER COMPILATION FAILED"

```
[Renderer] Compute shaders unavailable on this driver;
GPU cluster culling disabled (rendering is unaffected).
```

**Not an error.** Compute shaders need GL 4.3+ hardware support; older GPUs (Intel HD
3000 and similar) skip that optimisation. Rendering is fully correct.

### TDR / GPU timeout — screen freezes, resets, or the app is killed

The driver has a watchdog that resets the GPU when a single draw takes too long. On
weak hardware, Lithium's heavier passes (SSR, SSGI, PCSS, volumetrics, all at full
resolution) can exceed it.

Symptoms: momentary black screen, desktop flicker, "GPU hang" in `dmesg`, or the
process dying without a log entry.

```bash
sudo dmesg | grep -iE "gpu hang|reset|timeout|GPU HANG"
```

Fixes, cheapest first:

1. **Reduce the window size.** These are per-pixel costs; resolution is the dominant
   factor. Options → Resolution → 1280×720.
2. **Turn off the expensive passes.** Options → Rendering: disable **SSR**, then set
   Sun → Global Illumination to **Off**.
3. **Simplify the scene** — fewer SLR volumes; they raymarch per pixel.
4. **Raise the watchdog** (Intel i915), only if you understand the trade-off — a real
   hang will now freeze longer instead of recovering:
   ```bash
   sudo modprobe i915 enable_hangcheck=0        # until reboot
   ```
5. **Software rendering** to rule out the driver entirely (very slow, but it will run):
   ```bash
   LIBGL_ALWAYS_SOFTWARE=1 ./launch_engine.sh
   ```

### Black window / no rendering, but the process is alive

Check the log for shader link failures:

```bash
grep -E "FAILED|LINK" LithiumEngine_Startup.log
```

A failed shader link leaves that pass disabled. Report the log — this is an engine
bug, not a configuration problem.

---

## 3. Verifying file paths

The engine anchors its working directory to the executable at startup, so it finds its
assets regardless of where you launch from. If assets are missing, the layout is wrong.

### Expected layout

```
Lithium_Engine          <- binary
lib/                    <- 4 bundled .so files
EngineContent/
├── DefaultSky.hdr
├── NightSky.hdr
└── Skies/              <- sky library
Content/
├── DamagedHelmet.mesh
├── Sofa_01.gltf, Sofa_01.bin
├── Textures/
└── textures/
```

### Check it

```bash
ls Lithium_Engine lib/ EngineContent/DefaultSky.hdr Content/DamagedHelmet.mesh
```

### Confirm libraries resolve relative to the binary, not to an absolute path

```bash
readelf -d Lithium_Engine | grep RUNPATH
# Correct:   $ORIGIN/lib:$ORIGIN
# Broken:    /home/someone/...
```

`$ORIGIN` is expanded by the loader relative to the executable. Checking with
`ldd | grep /home` is **misleading** — inside its own folder the bundled libraries
legitimately resolve under your home directory. The real test is that the paths follow
the binary when you move it:

```bash
cp -r Lithium_Engine lib EngineContent Content /tmp/ptest/
ldd /tmp/ptest/Lithium_Engine | grep embree     # must say /tmp/ptest/lib/...
```

### Common path mistakes

| Symptom | Cause |
| --- | --- |
| `Failed to load HDRI environment map` | `EngineContent/` missing or renamed |
| `Sofa model unavailable; skipping` | `Content/Sofa_01.gltf` absent |
| Sky library empty in the picker | No `.hdr` files in `EngineContent/Skies` |
| Everything missing | Binary was copied out on its own, without its folders |

---

## 4. AppImage-specific

| Symptom | Fix |
| --- | --- |
| `dlopen(): error loading libfuse.so.2` | `sudo apt-get install libfuse2`, or run with `--appimage-extract-and-run` |
| `Permission denied` | `chmod +x Lithium_Engine-x86_64.AppImage` |
| Cannot save settings or bakes | The image is read-only; run from a writable directory — files are written to your current directory |

---

## 5. Editor issues

| Symptom | Status |
| --- | --- |
| Right-drag does not move the camera until you click an actor | **Known bug**, unfixed |
| Rotate gizmo mis-rotates an already-rotated object | **Known bug** — Euler order mismatch. Translate and scale are fine |
| Number fields need several clicks | Fixed — single click enters text edit |
| Ctrl+A / Ctrl+D fire while typing in a field | Fixed — shortcuts suppressed while a widget has focus |
| Scroll wheel moves the camera while scrolling a panel | Fixed — panels claim the wheel first |

---

## 6. Reporting a problem

Include:

1. `LithiumEngine_Startup.log`
2. `glxinfo | grep "OpenGL core profile version"`
3. Distribution and kernel — `uname -a`
4. `ldd Lithium_Engine | grep "not found"`
5. Whether you used the portable folder or the AppImage
