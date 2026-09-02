# Lithium Engine — System Requirements

**Linux x86-64 only.** No Windows or macOS build.

---

## Minimum — verified

This is not an estimate. The engine is developed and tested on this machine:

| | |
| --- | --- |
| **CPU** | Intel Core i5/i7 2nd gen (Sandy Bridge, 2011) — 4 threads |
| **GPU** | Intel HD Graphics 3000 (SNB GT2), integrated |
| **Driver** | Mesa 26.0 reporting **OpenGL 4.5 (Core Profile)** |
| **RAM** | 4 GB |
| **Disk** | 700 MB (engine + 200-HDRI sky library) |
| **Display** | X11 or Wayland (XWayland) |

### What that machine actually delivers

| Scene | FPS |
| --- | --- |
| Blank starter (helmet + primitives) | 15–30 |
| Reflections Showcase interior | 12–20 |
| Heavy SSR + SSGI + volumetrics | 1–5 |

**Compute shaders and fragment-shader image writes are unavailable at this tier.**
GPU cluster culling is skipped, and VXGI cannot run — this GPU reports zero fragment
image uniforms, so voxelisation is impossible and Global Illumination falls back to
SSGI. Both are logged at startup. Rendering is correct; only throughput and the
choice of GI technique are affected.

Startup is around 1 second. Linked programs are cached to disk, so only the first
launch after a shader change pays for compilation.

---

## Recommended — mid-range

| | |
| --- | --- |
| **CPU** | 6-core or better, 2017 or newer |
| **GPU** | Anything with a real OpenGL 4.5 driver and ~2 GB VRAM — GTX 1060, RX 580, Intel Arc, or a recent iGPU |
| **RAM** | 8 GB |
| **Disk** | 1 GB SSD |

Expect the sample scenes at 60+ FPS with every effect enabled. Compute shaders become
available, so cluster culling switches on automatically.

---

## High-end — headroom, not requirement

| | |
| --- | --- |
| **CPU** | AMD Ryzen 9 9950X3D or Intel Core Ultra 9 |
| **GPU** | NVIDIA RTX 5080 / AMD RX 8900 class |
| **RAM** | 32 GB |

Worth being straight about what this buys you today: **the renderer will not saturate
hardware at this tier.** It has no render graph, no dynamic resolution, no GPU-driven
culling beyond the single cluster-culling compute pass, and every post-process runs at
full resolution. A 5080 will render the sample scenes at very high frame rates and
spend most of its time idle.

It does **not** unlock ray tracing. That needs a Vulkan/DX12 backend the engine does
not have — see `Engine_Limitations.md`. Selecting *Hardware Ray Tracing* in the GI
dropdown runs SSGI on any GPU, including an RTX 5080.

The parallelised environment convolution at startup does scale with core count, so a
high-end CPU meaningfully cuts load time.

### If a Vulkan backend lands

It would be new work, not a switch to flip. An `RHI::RendererAPI` abstraction and a
stubbed Vulkan backend used to exist here; both were removed because `renderer.cpp`
never referenced them once, and selecting *Vulkan* in the editor destroyed the GL
context and rebuilt an identical one while writing `graphics_api: vulkan` into the
config. An abstraction that has only ever had one implementation does not describe
what a second would need.

VXGI is implemented and will run here, unlike on the minimum-target part: voxel cone
tracing needs image load/store, which any GPU of this generation has in abundance.
Grid resolution, extent and intensity are exposed in the editor.

The GI mode enum does still reserve a hardware-RT tier, which is unimplemented and
needs a Vulkan or DX12 backend that does not exist. **Treat it as a future tier, not
a shipping feature.**

---

## Required system libraries

Bundled with the engine — do not install:

`libembree4.so.4` · `libsycl.so.8` · `libtbb.so.12` · `libur_loader.so.0`

Must come from your distribution (never bundle these — a bundled `libGL` cannot reach
your kernel driver, and a bundled glibc will not load against a different host):

| Library | Debian/Ubuntu | Arch | Fedora |
| --- | --- | --- | --- |
| OpenGL | `libgl1 libglx-mesa0 libglvnd0` | `libglvnd mesa` | `libglvnd-glx mesa-libGL` |
| SDL2 | `libsdl2-2.0-0` | `sdl2` | `SDL2` |
| X11 | `libx11-6 libx11-xcb1` | `libx11 libxcb` | `libX11 libxcb` |
| Wayland | `libwayland-client0 libwayland-egl1` | `wayland` | `wayland-libs` |
| Audio | `libasound2 libpulse0` | `alsa-lib libpulse` | `alsa-lib pulseaudio-libs` |
| C++ runtime | `libstdc++6 libgcc-s1` | `gcc-libs` | `libstdc++ libgcc` |

`launch_engine.sh` detects anything missing and offers the correct command for your
distribution — you should not need this table.

---

## Checking your system

```bash
glxinfo | grep "OpenGL core profile version"   # needs 4.5 or higher
nproc                                          # cores available to the bake/convolution
ldd Lithium_Engine | grep "not found"          # should print nothing
```

If `glxinfo` is unavailable: `sudo apt-get install mesa-utils` (or your distro's
equivalent).
