# Lithium Engine — Known Limitations

Written plainly so nobody is surprised mid-project. Lithium is a **hobby engine**, not
a production one. Much of it works well; the parts that do not are listed here rather
than discovered later.

---

## Status

Single-developer hobby project. No API stability guarantees, no LTS branch, no
migration tooling between versions. Scene files (`.lithium`) may change format without
a converter. **Keep backups of anything you care about.**

Platform: **Linux x86-64**, with a Windows build that runs. No macOS build.
Graphics: **OpenGL 4.5 only.** There is no Vulkan or DirectX backend.

---

## Global illumination — what is real and what is not

The GI dropdown (Sun → Global Illumination) offers four modes. Three are implemented;
the fourth has no backend:

| Mode | Status |
| --- | --- |
| **Off / Disabled** | Real. Disables indirect bounce; ambient occlusion stays on. |
| **Screen-Space GI (SSGI)** | Real. Half-hemisphere stochastic bounce with firefly clamping and a depth-aware blur. |
| **Voxel Cone Tracing (VXGI)** | Real, but **needs hardware this engine's minimum target does not have.** See below. |
| **Hardware Ray Tracing** | **Not implemented.** Selecting it runs SSGI. |

VXGI is implemented: the scene is voxelised into a 3D grid each time the volume goes
stale, the grid is mip-filtered, and the lighting pass gathers indirect light with six
cones per pixel. Because a cone walks the grid rather than the screen, it picks up
light from geometry that is off-screen or hidden — the thing SSGI structurally cannot
do.

It requires image load/store from a fragment shader. **The minimum-target GPU (Intel
HD 3000) reports `GL_MAX_FRAGMENT_IMAGE_UNIFORMS = 0`** — it cannot write an image
from a fragment shader at all, so voxelisation cannot run there. The renderer probes
that limit at startup and falls back to SSGI with a line saying so:

```
[Renderer] This GPU allows 0 fragment and 0 compute image uniforms; VXGI needs at
least 1 and 2. Global Illumination falls back to SSGI.
```

On hardware that does support it, the editor exposes grid resolution (32–256³),
world extent, intensity and a manual rebuild.

Hardware RT still needs a Vulkan/DX12 backend that does not exist here. Both modes are
labelled in the editor; neither silently pretends to work.

### SSGI is screen-space, with the usual consequences

Indirect light only comes from what is currently on screen. Light bouncing off a
surface behind the camera, or off an object occluded by another, contributes nothing.
Turning the camera can visibly change indirect lighting. This is inherent to the
technique, not a bug.

---

## Static light baking

**Rendering → Bake Static Lighting** builds a real lightmap: charts are packed into a
UV atlas, texels are rasterised in texture space, and visibility is traced against a
BVH over the scene. The bake also fills an irradiance probe grid for anything not
lightmapped. Results are written to `Content/Bakes/` as `.lmap` files and reloaded
with the scene.

The remaining limits are the ordinary ones: it is an offline bake, so adding or moving
a light requires a re-bake, and atlas resolution bounds how fine a shadow the lightmap
can hold.

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
- **Shadows are cascaded** — four 2048² layers of a depth array, each fitted to the
  bounding sphere of its slice of the view frustum, with PCSS filtering and a
  cross-fade at each boundary. Resolution, distance, split distribution, blend width
  and both bias terms are exposed in the editor, along with a cascade-tint debug view.
  Surfaces past the shadow distance (250 units by default) are lit as unoccluded,
  faded in over the last tenth of the range.

---

## Editor bugs currently open

None of the previously listed ones. For the record, since they were documented here
for a long time:

- **Right-click camera movement** is fixed. `SDL_SetRelativeMouseMode` requires window
  input focus, and a right-click arriving while focus was elsewhere was delivered but
  refused relative mode, so no motion deltas ever arrived — which is why clicking the
  Outliner first appeared to "fix" it. The engine now takes focus explicitly, and
  falls back to integrating absolute cursor deltas if the compositor refuses.
- **The rotate gizmo** is fixed. Write-back goes through `Transform::from_relative_matrix`,
  an exact inverse of this engine's own `rotZ * rotX * rotY` composition, rather than
  through ImGuizmo's decomposition in a different Euler order.

## Performance expectations

Developed and tested largely on 2011 Intel HD 3000 integrated graphics, where the
sample scenes run at roughly **1–30 FPS** depending on which effects are enabled. The
renderer is not optimised: SSR, SSGI, PCSS and the bloom pyramid all run at full
resolution with no dynamic scaling. It will be substantially faster on modern hardware,
but do not expect the throughput of an engine with a mature render graph.

**Startup is ~1 second** to a ready renderer. Linked program binaries are cached to
disk via `glGetProgramBinary`, so only the first launch after a shader change pays
the compilation cost.
