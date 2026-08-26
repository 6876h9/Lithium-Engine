# DON'T LOOK AWAY

A short horror game built on Lithium.

```
Content/HorrorGame/
  facility.lithium        the level and the game: 44 actors, 16 collision volumes, 11 lights
  dont_look_away.cminus   all of the gameplay, in the engine's own scripting language
  gen_level.py            regenerates the level and the script's collision table together
  README.md               this file
```

---

## Running it

```
cd build
./Lithium_Game Content/HorrorGame/facility.lithium
```

The mouse is captured on the first frame and you can walk immediately — there is no
key to press to start. The scene carries the script, so that is the whole setup. The build copies
`Content/HorrorGame` next to both executables, so it also works from the editor:
`./Lithium_Engine Content/HorrorGame/facility.lithium` opens it for editing instead
of playing it.

---

## The game

You are in a dark facility. Four beacons are out. Light all four, then get back to
the lamp at the centre.

Something else is in here with you.

**It does not move while you are looking at it.** Turn your back, or let a wall come
between you, and it closes the distance. It is bound by the same walls you are, so
corners genuinely shelter you — and genuinely hide it. Every beacon you light makes
it faster: waking the facility wakes it too.

If it reaches you, you come to at the centre and one beacon has gone dark again.

**Controls**

| | |
|---|---|
| W A S D | walk |
| Escape | release the mouse |
| Left Shift | capture it again |

Four markers along the top of the screen track the beacons; a charged beacon's
fixture powers up in the world too. Everything else is felt rather than read — your
hands get less steady the closer it is, the screen bleeds red as it closes, and it
glows a little brighter when it knows you can see it. The console also prints `1`–`4`
for beacons, `-1` when it catches you and `999` when you get out.

---

## Engine changes

Three, all made because the game could not exist without them.

**1. `SceneSerializer` now round-trips script components.**
`actor_to_json` wrote mesh, material, transform, lights and physics, but had no
script field — so a saved scene came back as a static diorama, and
`Lithium_Game <scene>` (which loads a scene and goes straight to `PlayInEditor`)
had nothing to run. Scenes now carry `"script": {"type": ..., "path": ...}` for both
`CMinusComponent` and `CppScriptComponent`, attached last so the first tick sees a
fully built actor. *Without this the game cannot be launched at all* — free-camera
movement is explicitly suppressed in play mode, so a scene-only build is a diorama
you cannot move through.

**2. `find_actors` plus `set_nearby_color` / `set_nearby_emissive`.**
A script could read the whole scene and change none of it: `get_nearby_actors` is
centred on the script's own actor, and there was no setter for anything else. Since
the player here is not an actor at all — it is camera state inside the script —
an owner-relative query could never reach what the player was standing next to.
`find_actors(x, y, z, r)` fills the same cache from an arbitrary point, and the two
setters are the only way a script can alter an actor other than its own. This is
what makes a charged beacon light up.

**3. A script-drawable HUD: `hud_pip(index, on)` and `hud_vignette(amount, r, g, b)`.**
C-Minus has no way to draw, so a game written in one had no way to tell the player
what it wanted from them. The script writes into `Engine::ScriptHud` and
`Engine::draw_script_hud()` renders it through ImGui's foreground draw list, so it
sits over the viewport in the editor and over the fullscreen image in a standalone
build without either needing to know it exists. Pips are objectives; the vignette is
a screen-edge tint. Both are deliberately generic — no game strings in the engine.

**4. Scenes carry their own environment and sky settings.**
`sky_mode` and `void_color` live on the sun and were not serialised, and the HDRI was
whatever happened to be resident — the engine's default is a daytime sky. Its
image-based ambient does not care that there is a ceiling in the way, so this
interior loaded washed out and grey, with none of the contrast its lamps were meant
to carve. Scenes now write a top-level `"environment": {"hdri": ...}` and the sun's
`sky_mode` / `void_color` / `enable_3d_clouds`. This one lights itself with
`EngineContent/NightSky.hdr` against a flat near-black background.

Also: `CMakeLists.txt` stages `Content/HorrorGame` beside both binaries, because the
scene names the script by a path relative to the working directory.

## Why the whole game is one script, on the monster

A C-Minus script can now read and tint any actor, but it can still only *move* the
actor it is attached to. The camera is the exception — `set_cam_pos` / `set_cam_rot`
go through `g_engine` and work from anywhere. So the player is not an actor: they
are four variables inside the script, pushed to the camera every frame.

That leaves exactly one thing in the level that has to be *moved* rather than looked
at: the stalker. So the script lives on the stalker, and everything else — you, your
collisions, the beacons, the win condition — is state it carries.

A script per moving actor would not work either: each `CppScriptComponent` compiles
to its own shared library, so two scripts cannot share state, and C++ scripts cannot
reach `g_engine` at all — `core/engine.hpp` includes `core/editor.hpp` →
`visual_script_editor.hpp` → `imnodes.h`, and the script compile line is only
`-I./include -I../include -I./src -I../src`.

---

## What was verified

19/19, against the shipping files, using the engine's own serializer, lexer, parser
and interpreter.

**The scene carries the game**

| Check | Result |
|---|---|
| Scene loads through `SceneSerializer::load_scene` | pass |
| Script component deserialised onto the Stalker | pass |
| Script compiled on attach | no parse error |
| Survives `save_scene` → `load_scene` | pass |
| Scene declares its own environment | `NightSky.hdr` |
| Sun carries `sky_mode` / `void_color` | `sky_mode 2` |

**Gameplay**

| Check | Result |
|---|---|
| Playable with no keypress at all | `state=1`, mouse captured, after one frame |
| W walks immediately | **1.35 units** in 0.5s |
| W/D match the engine's own camera basis | **0.0000** deviation over 7 headings |
| Player never clips a wall | 32 headings × 400 frames, **0 breaches** |
| Observed ⇒ frozen | moved **0.0000** units while watched |
| Unobserved ⇒ closes in | **3.10** units in 2s |
| All four beacons charge; hub ends the run | pass |
| Stalker stays in the level | **0 / 2000** frames embedded |

The level and the script's collision table are generated together by `gen_level.py`,
and the regenerated scene is byte-identical to the shipped one — so a wall cannot be
visible without also being solid.

**Not verified by me:** the look. Everything above runs headless. The HUD draws (you
can see the four pips in the first screenshot), but whether the night environment
actually fixes the washed-out lighting is a judgement I could not make without
running it.

---

## Known limitations

- **No audio.** `AudioComponent` exists but nothing in the script API can trigger it,
  and the scene format does not serialise it.
- **No flashlight.** A script can only move its own actor, and its own actor is the
  monster. Lighting is fixed: beacons, four failing strip lights, and the lamp at the
  centre.
- **Collision is axis-aligned boxes** hand-mirrored from the level, not the physics
  engine. `raycast()` goes through Jolt and these walls have no rigid bodies, so it
  would report a clear path straight through every one of them. Line of sight is
  marched by hand for the same reason.
- **You can walk through the stalker's body**; contact is a radius check.
- `Content/fps_controller.cminus`, the bundled sample, had a mirrored camera basis
  (`forward = (+sin, 0, -cos)`, `right = (cos, 0, +sin)`). This game inherited it from
  there. Both are fixed; if you have other scripts derived from that sample they have
  the same bug.
- **The HUD is numeric and graphical only** — pips and a tint. C-Minus has no strings,
  so there is no way to put a sentence on screen without hardcoding game text into
  the engine.
