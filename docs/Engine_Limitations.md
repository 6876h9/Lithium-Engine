# Lithium Engine — Known Limitations

Written plainly so nobody is surprised mid-project. Lithium is a **hobby engine**, not
a production one. Much of it works well; the parts that do not are listed here rather
than discovered later.

---

## Status

Single-developer hobby project. No API stability guarantees, no LTS branch, no
migration tooling between versions. Scene files (`.lithium`) may change format without
a converter. **Keep backups of anything you care about.**

Platform: **Linux x86-64 only.** No Windows or macOS build.
Graphics: **OpenGL 4.5 only.** There is no Vulkan or DirectX backend.

---

## Global illumination — what is real and what is not

The GI dropdown (Sun → Global Illumination) offers four tiers. Two are implemented:

| Mode | Status |
| --- | --- |
| **Off / Disabled** | Real. Disables indirect bounce; ambient occlusion stays on. |
| **Screen-Space GI (SSGI)** | Real. Half-hemisphere stochastic bounce with firefly clamping and a depth-aware blur. |
| **Voxel Cone Tracing (VXGI)** | **Not implemented.** Selecting it runs SSGI. |
| **Hardware Ray Tracing** | **Not implemented.** Selecting it runs SSGI. |

VXGI needs a scene voxelization pass, a sparse 3D texture and cone marching — a
subsystem in its own right. Hardware RT needs a Vulkan/DX12 backend that does not
exist here. Both are wired through the UI and clearly labelled as unavailable in the
editor; neither silently pretends to work.

### SSGI is screen-space, with the usual consequences

Indirect light only comes from what is currently on screen. Light bouncing off a
surface behind the camera, or off an object occluded by another, contributes nothing.
Turning the camera can visibly change indirect lighting. This is inherent to the
technique, not a bug.

---

## Static light baking is per-object, not lightmapped

**Rendering → Bake Static Lighting** (also in the Sun's Details panel) computes **one
irradiance value per actor** marked *Static*, and applies it as albedo-modulated
bounce.

It is **not** a lightmap pipeline. There is no UV atlas generation, no texel-space
rasterisation, no lightmap textures. Practical consequences:

- Lighting is uniform across each object. A long wall receives one value, so it cannot
  be bright at one end and shadowed at the other.
- Small objects read correctly; large ones read flat.
- Adding or moving lights requires a re-bake; nothing updates automatically.

Real lightmapping needs per-mesh UV unwrapping (xatlas or equivalent) plus a texture
cache. That is the honest gap between this and an engine like Unity or Unreal.

---

## Object-space visibility and caching

Three separate mechanisms, none of them a full visibility system:

**1. Bake-time occlusion (object-space).** During a bake, visibility toward each light
is tested by marching against other static actors approximated as **bounding spheres**.
Coarse by design: it captures "this object sits behind that one" and attenuates to 25%
rather than to black. Concave geometry and thin occluders are not represented — a
sphere around a wall occludes far more than the wall does.

**2. World partition chunks.** Actors more than one 100-unit chunk from the camera are
skipped for ticking and rendering. Off by default; enabling it in a scene whose objects
are spread beyond that radius will make distant ones disappear.

**3. GPU cluster culling — disabled on most hardware.** A compute shader performs
frustum culling per mesh cluster. Compute shaders require GL 4.3+; on older drivers
(including Intel HD 3000, the minimum target) it is unavailable and the engine logs:

```
[Renderer] Compute shaders unavailable on this driver; GPU cluster culling disabled
(rendering is unaffected).
```

Rendering is correct either way — this is a throughput optimisation, not a
correctness feature.

---

## Renderer limitations

- **SSR is screen-space.** Reflections of off-screen geometry do not exist; reflections
  fade toward screen edges. A mirror floor cannot reflect what the camera cannot see.
- **Specular IBL uses a CPU-prefiltered chain** built at load. Higher startup cost, and
  it is regenerated whenever the environment map changes.
- **Cube normals are position-normalised**, not per-face, so primitive cubes shade with
  a slightly rounded look rather than crisp flat faces.
- **Environment maps are clamped to 8192** on load. HDRIs store sun radiance far above
  the half-float ceiling of 65504, and anything over it became `+Inf`, propagated
  through mip generation, and turned every lit pixel into `NaN`. The clamp is what
  prevents that; it also slightly dims extremely bright suns in reflections.
- **Shadows are a single map**, not cascaded — one 4096² map fitted to a 40-unit radius
  around the camera. Objects beyond that receive no shadows.

---

## Editor bugs currently open

- **Right-click camera movement is unreliable.** Right-drag to look/fly sometimes does
  not respond until you click something in the Outliner. Not yet fixed.
- **Rotate gizmo mis-rotates already-rotated objects.** `DecomposeMatrixToComponents`
  returns Euler angles in ImGuizmo's order, while `Transform` recomposes as
  `rotZ * rotX * rotY`. Translate and scale are unaffected (they no longer write
  rotation back), but the rotate gizmo still round-trips through the mismatch.

---

## Performance expectations

Developed and tested largely on 2011 Intel HD 3000 integrated graphics, where the
sample scenes run at roughly **1–30 FPS** depending on which effects are enabled. The
renderer is not optimised: SSR, SSGI, PCSS and the bloom pyramid all run at full
resolution with no dynamic scaling. It will be substantially faster on modern hardware,
but do not expect the throughput of an engine with a mature render graph.

**Startup is ~4 seconds**, dominated by shader compilation. Compiled program binaries
are not cached to disk, so every launch pays that cost.
