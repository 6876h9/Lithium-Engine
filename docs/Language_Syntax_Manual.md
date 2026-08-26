# C-Minus & V-Script — Syntax Manual

Two ways to script actor behaviour:

- **C-Minus** — a small interpreted text language, one `.cminus` file per script.
- **V-Script** — a node graph editor that drives the same runtime.

---

# Part 1 — C-Minus

## The type system

There is exactly one type: **32-bit float**. No integers, strings, booleans, structs
or objects. Booleans are floats, where `0.0` is false and anything else is true.

Every variable and array element is a float. This is a deliberate simplification — it
keeps the interpreter small, and it is the first thing to know before writing anything.

## Comments

```c
// Line comment, runs to end of line.
```

`/* block comments */` are **not** supported.

## Variables

No declaration keyword and no type. Assigning creates.

```c
speed = 2.5
angle = speed * 2.0
```

Uninitialised variables read as `0.0`.

## Arrays

Indexed with `[ ]`. Indices are floats, truncated to integers.

```c
waypoints[0] = 10.0
waypoints[1] = 25.5
current = waypoints[0]
```

Arrays grow on assignment. There is no length operator and no bounds error.

## Operators

Highest precedence first:

| Category | Operators |
| --- | --- |
| Unary | `-` `!` |
| Multiplicative | `*` `/` `%` |
| Additive | `+` `-` |
| Relational | `<` `<=` `>` `>=` |
| Equality | `==` `!=` |
| Logical AND | `&&` |
| Logical OR | `\|\|` |

Comparisons yield `1.0` or `0.0`.

## Control flow

```c
if (health < 20.0) {
    set_color(1.0, 0.0, 0.0)
} else {
    set_color(1.0, 1.0, 1.0)
}
```

```c
i = 0
while (i < 5) {
    print(i)
    i = i + 1
}
```

Braces are required for multi-statement bodies. Semicolons are optional.

## What C-Minus does not have

No user-defined functions, no `for` loop, no `break`/`continue`, no `return`, no
strings, no imports. A script is a flat list of statements re-executed each tick.

---

## Built-in function reference

### Actor transform

| Function | Meaning |
| --- | --- |
| `set_position(x, y, z)` | Move to world position |
| `set_rotation(x, y, z)` | Set Euler rotation, radians |
| `set_scale(x, y, z)` | Set scale |
| `get_x()` `get_y()` `get_z()` | Current position component |
| `get_rot_x()` `get_rot_y()` `get_rot_z()` | Current rotation component |
| `look_at(x, y, z)` | Face a world point |

### Material

| Function | Meaning |
| --- | --- |
| `set_color(r, g, b)` | Albedo, 0–1 |
| `set_metallic(v)` | 0 dielectric, 1 metal |
| `set_roughness(v)` | 0 mirror, 1 fully rough |
| `set_emission(v)` | Self-illumination; above ~1 it blooms |

### Motion helpers

| Function | Meaning |
| --- | --- |
| `oscillate(freq, amp)` | Sine wave over time |
| `orbit(cx, cy, cz, radius, speed)` | Circle a point |
| `spring(current, target, stiffness)` | Damped approach toward target |
| `lerp(a, b, t)` | Linear blend |
| `smoothstep(a, b, t)` | Eased blend |

### Maths

`sin` `cos` `tan` `sqrt` `pow` `abs` `min` `max` `clamp` `rand` `dot` `distance`

### Time & input

| Function | Meaning |
| --- | --- |
| `get_time()` / `time` | Seconds since start |
| `get_dt()` / `dt` | Delta time for this frame |
| `is_key_pressed(code)` | Non-zero while held |

**Always scale movement by `dt`**, or speed changes with frame rate:

```c
set_position(get_x() + 2.0 * dt, get_y(), get_z())
```

### World queries & physics

| Function | Meaning |
| --- | --- |
| `get_nearby_actors(radius)` | Count nearby; fills the nearby cache |
| `get_nearby_x(i)` `get_nearby_y(i)` `get_nearby_z(i)` | Position of cached actor `i` |
| `raycast(ox, oy, oz, dx, dy, dz)` | Distance to first hit |
| `apply_force(x, y, z)` | Physics impulse |
| `spawn_actor(type, x, y, z)` | Spawn at position |
| `destroy_self()` | Remove this actor |

### Camera & rendering

`set_cam_pos(x,y,z)` · `set_cam_rot(x,y,z)` · `set_time_of_day(t)` ·
`set_lighting(v)` · `set_wireframe(v)` · `set_msaa(v)`

### Debug

`print(value)` — writes to `LithiumEngine_Startup.log`, not the terminal.

---

## Worked example

```c
// Bob gently and pulse red when something comes close.
base_y   = 1.0
bob      = oscillate(2.0, 0.25)
set_position(get_x(), base_y + bob, get_z())

count = get_nearby_actors(5.0)
if (count > 0.0) {
    d = distance(get_x(), get_y(), get_z(),
                 get_nearby_x(0), get_nearby_y(0), get_nearby_z(0))
    heat = clamp(1.0 - d / 5.0, 0.0, 1.0)
    set_color(1.0, 1.0 - heat, 1.0 - heat)
    set_emission(heat * 2.0)
} else {
    set_color(1.0, 1.0, 1.0)
    set_emission(0.0)
}
```

---

# Part 2 — V-Script

A node graph over the same built-ins. Open it from the editor and attach it to an
actor; the graph is evaluated per tick exactly like a C-Minus script.

## Node categories

**Transform** — Set Position, Set Rotation, Set Scale, Look At
**Material** — Set Metallic, Set Roughness, Set Emission, Rainbow Color Cycle
**Motion** — Sine Bobbing, Orbit, Spring, Random Wander
**Logic & data** — Set Variable, Math Expression, Array Access, Array Assign, While
**World** — Spawn Actor, Destroy Self, Get Nearby Actors, Raycast, Apply Force
**Input** — If Key Pressed
**Camera & render** — Set Camera Pos, Set Camera Rot, Set Time of Day, Set Lighting,
Set Wireframe, Set MSAA
**Utility** — Clamp

## Choosing between them

Graphs read well for short behaviours — bob, spin, react to a key. Past roughly a
dozen nodes, or anywhere with real branching and arithmetic, C-Minus is easier to
follow and to diff. The **Math Expression** node accepts a C-Minus expression, so a
graph can call down into the text language where a formula is clearer than wiring.
