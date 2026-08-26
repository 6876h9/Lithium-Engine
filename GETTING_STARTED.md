# Getting Started with Lithium Engine Development

This guide covers building gameplay in the engine once it's running — see
[README.md](README.md) for how to install, build, and export. It documents
what's actually implemented in this codebase (`src/`, `include/`), not a
roadmap.

## Core concepts

Every object placed in a scene is an `Actor` (`include/world/actor.hpp`).
An `Actor` owns a list of `ActorComponent`s that give it behavior — a mesh
to render, a collider to simulate, a script to run. One component may be
set as the actor's `root_component` (must be a `SceneComponent`, which adds
a `Transform`).

```cpp
auto* actor = new_actor.get(); // an Actor you've created/spawned
auto* mesh_comp = actor->create_component<StaticMeshComponent>("Mesh");
actor->set_root_component(mesh_comp);

auto* physics = actor->get_component<PhysicsAttribute>(); // nullptr if none exists
actor->remove_component(physics);
```

`create_component<T>`, `get_component<T>`, and `remove_component` are all
you need for composing behavior — there's no separate registration step.

## Importing 3D models (Asset Pipeline)

Right-click the **Content Browser** and choose **Import 3D Model...** to
pick an FBX, glTF/GLB, OBJ, DAE, or Blender file. Under the hood
(`ModelImporter::import_model`, `src/core/model_importer.cpp`):

- The full Assimp node hierarchy is walked and merged into one mesh, with
  each node's transform baked into the vertex positions/normals — multi-part
  models import correctly instead of only keeping the first submesh.
- UVs are read from the first UV channel.
- The first material with a resolvable diffuse/base-color texture wins:
  embedded textures are extracted into `Content/Textures/`, external
  texture files are copied there. Materials with no texture use their base
  color as a flat per-vertex tint.
- The result is written to `Content/<name>.mesh`, a small custom binary
  format (see `ModelImporter::load_mesh_file` for the layout).

Drag the resulting `.mesh` file from the Content Browser into the viewport
to spawn an `Actor` with a `StaticMeshComponent` pointing at it. The
diffuse texture (if any) loads asynchronously and appears once ready — no
extra wiring needed.

## Adding physics

`PhysicsAttribute` (`include/world/physics_attribute.hpp`) wraps a Jolt
Physics rigid body:

```cpp
auto* physics = actor->create_component<PhysicsAttribute>("Physics");
physics->mass = 2.0f;
physics->collider_type = 0;               // 0 = Box, 1 = Sphere
physics->box_half_extents = {0.5f, 1.0f, 0.5f};
physics->simulate_gravity = true;
```

These are also exposed as editable fields in the editor's Details panel
when a `PhysicsAttribute` is attached to a selected actor.

## Writing gameplay code (native C++ components)

`CppScriptComponent` (`include/world/cpp_script_component.hpp`) compiles a
`.cpp` file into a shared library with `g++` and hot-loads it with
`dlopen` — no engine rebuild required. Your script exports two optional
`extern "C"` entry points:

```cpp
// Content/Scripts/my_behavior.cpp
#include "world/actor.hpp"

extern "C" void on_begin_play(Actor* owner) {
    // runs once when the actor spawns
}

extern "C" void on_tick(Actor* owner, float delta_time) {
    owner->get_actor_transform().position.y += delta_time; // e.g. float upward
}
```

Attach it in code or via the editor:

```cpp
auto* script = actor->create_component<CppScriptComponent>("Behavior", "Content/Scripts/my_behavior.cpp");
if (script->has_error) {
    std::cerr << script->build_log << std::endl; // compiler output on failure
}
```

Because compilation shells out to `g++ -I./include -I../include -I./src -I../src`,
run the engine from a directory where one of those include paths resolves
(the `build/` directory works, since `../include` and `../src` then point
at the project's real headers).

## Visual scripting (C-Minus)

For non-C++ workflows, `.cminus` files (see `CRASH_COURSE`-style docs
inside `src/scripting/cminus_interpreter.cpp`) provide a simpler
declarative command syntax (e.g. `new.block(...)`, `nightmode();`),
editable through the node-based Visual Script Editor
(`src/core/visual_script_editor.cpp`, built on ImNodes).

## Materials

`.material` files (`include/world/material.hpp`) are plain key=value text
files describing a full PBR parameter set (albedo, metallic, roughness,
transmission, subsurface, etc.). Create one from the Content Browser's
right-click menu, or drag one onto a selected actor to apply it.

## Where things live

| Concern | Files |
|---|---|
| Actors & components | `include/world/`, `src/world/` |
| Rendering (deferred GL pipeline, RHI split) | `src/renderer/`, `include/renderer/` |
| Physics (Jolt) | `src/physics/`, `include/physics/` |
| Asset import | `src/core/model_importer.cpp`, `src/core/texture_resource.cpp` |
| Resource loading/streaming | `src/core/resource_manager.cpp` |
| Editor UI | `src/core/editor.cpp` |
| Scripting | `src/world/cpp_script_component.cpp`, `src/scripting/cminus_interpreter.cpp` |
