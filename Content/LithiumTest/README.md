# LITHIUM TEST

The sample game for the Lithium engine. A short horror piece that exists to
exercise the engine end to end — rendering, physics collision, scripting, audio,
HUD and input — in something you actually play rather than something you read
about.

```
Content/LithiumTest/
  lithium_test.lithium          the level and the game: 30 actors, 2 collision volumes, 11 lights
  lithium_test.cminus           all of the gameplay, in the engine's own scripting language
  lithium_test.lithium.strings  the lines it puts on screen
  lithium_test.lithium.sounds   the cues it plays
  audio/                        four generated sound cues
  gen_level.py                  regenerates the level and the script's collision table together
  README.md                     this file
```

---

## As you see it

You come to in a dark hall. Not pitch black — a lamp burns at the centre and four
failing strip lights stutter somewhere overhead — but dark enough that the far
corners are guesses. A line of text tells you what to do and then gets out of the
way: **light the four beacons, then come back to the lamp.** Four small markers
sit along the top of the screen, unlit.

You walk. The mouse is already yours; nothing asks you to press start. The space
is wide and almost empty — two long walls run across it, north and south, and
between them there is nothing but floor. The night sky through the ceiling is
doing the ambient lighting, so surfaces facing up are cool and faintly blue and
everything facing away is nearly black.

Then you notice you are not alone.

It is standing across the hall, and it is looking at you. **It does not move while
you are looking at it.** That is the whole game. Watch it and it is a statue.
Glance away to find the next beacon and it is closer — not sliding, not animating
toward you, just *closer*, the way a thing in a dream is closer.

There is almost nowhere to break its line of sight. Two walls is all the cover
this hall has, and it wants the open ground as much as you do.

Reaching a beacon lights it. The fixture powers up in the world, a marker fills at
the top of the screen, a chime plays, and the thing gets faster. Waking the hall
wakes it too. By the fourth beacon it is crossing open ground at a pace you cannot
outwalk in a straight line, and with this little cover the only reason you are
still moving is that you have learned to reverse, look, and reverse again.

Nothing tells you how close it is in numbers. Your view starts to drift — your
hands, less steady — and the edges of the screen bleed red as it closes. When it
is certain you can see it, it glows a shade brighter, which is somehow worse.

If it reaches you, the screen goes and you come to at the centre again with one
beacon dark. Recoverable. Humiliating.

Light all four and the tint turns warm, the markers all sit full, and the only
thing left is the walk back to the lamp — straight across open floor, with the
fastest version of it awake behind you.

---

## Controls

| | |
|---|---|
| W A S D | walk |
| Mouse | look — captured on the first frame |
| Escape | release the mouse |
| Left Shift | sprint (1.5x) — and recapture the mouse when it is loose |

**If it is too dark to see**, the game is meant to be dark, but panels vary.
Options → **Brightness**, above 1.00x.

---

## Running it

```
./Lithium_Game Content/LithiumTest/lithium_test.lithium     # play it
./Lithium_Engine Content/LithiumTest/lithium_test.lithium   # open it for editing
```

The build stages `Content/LithiumTest` beside both executables, so both work from
the build directory.

---

## What it exercises

The game is a test in the literal sense — every one of these is an engine path
that a plain launch never touches:

| Engine feature | How the game uses it |
|---|---|
| Deferred PBR rendering | the whole level, lit by 11 lights and an HDRI |
| HDRI image-based lighting | scene-carried `NightSky.hdr` doing the ambient |
| C-Minus scripting | all gameplay, one script |
| Script HUD | four objective pips and a screen-edge tint |
| Script string table | every line of text you see |
| Script audio | four one-shot cues |
| Camera control from script | you are camera state, not an actor |
| Emissive material control | beacons powering up |
| Scene serialization | the scene carries its script and environment |

---

## How it is built

The player is not an actor. A C-Minus script can only *move* the actor it is
attached to, and the one thing that must move is the stalker — so the script lives
on the stalker, and you are four variables inside it pushed to the camera every
frame through `set_cam_pos` / `set_cam_rot`.

The level and the script's collision table are generated together by
`gen_level.py`, so a wall cannot be visible without also being solid. Regenerate
both with:

```
python3 Content/LithiumTest/gen_level.py
```

---

## Known limitations

- **The map is open on the east and west edges.** The level is down to two walls,
  and those walls are the only collision in it — there is no separate arena bound,
  so you can walk off the floor. Add walls back in `gen_level.py` if you want it
  closed again.
- **Collision is axis-aligned boxes** hand-mirrored from the level, not Jolt. The
  walls have no rigid bodies, so `raycast()` would report a clear path straight
  through them; line of sight is marched by hand for the same reason.
- **You can walk through the stalker.** Contact is a radius check.
- **No flashlight.** A script can only move its own actor, and its own actor is the
  monster. Lighting is fixed.
- **The HUD is one line at a time.** Enough to brief you; not a dialogue system.
