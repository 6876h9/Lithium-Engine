#!/usr/bin/env python3
"""Regenerates lithium_test.lithium.

The wall list below is the single source of truth for the level: this script emits
both the scene and the collision table that lithium_test.cminus carries, so a wall
cannot exist visually without also being solid. If you move a wall here, paste the
printed table back over the one in the script's init block.

    python3 Content/LithiumTest/gen_level.py
"""

import json
import os

# (name, centre_x, centre_z, size_x, size_z) - every wall is 4 tall, centred at y=2
WALLS = [
    # Two walls only. The perimeter and the cross of stubs were removed, so this is
    # an open arena crossed by a single north and a single south wall.
    #
    # These are also the only collision in the level - there is no separate arena
    # bound - so the east and west edges are open and you can walk off the floor.
    ("Wall_North",     0.0, -20.0, 41.0,  1.0),
    ("Wall_South",     0.0,  20.0, 41.0,  1.0),
]
BEACONS = [
    ("Beacon_NW", -15.5, -15.5),
    ("Beacon_NE",  15.5, -15.5),
    ("Beacon_SW", -15.5,  15.5),
    ("Beacon_SE",  15.5,  15.5),
]

STRIPS = [(0.0, -7.0), (0.0, 7.0), (-7.0, 0.0), (7.0, 0.0)]

WALL_H = 4.0
SCRIPT_PATH = "Content/LithiumTest/lithium_test.cminus"
HERE = os.path.dirname(os.path.abspath(__file__))


def actor(name, shape, pos, scale, color, *, metallic=0.0, roughness=0.85,
          rot=(0.0, 0.0, 0.0), light=None, invisible=False):
    a = {
        "name": name,
        "shape_type": shape,
        "mesh_path": "",
        "material_path": "",
        "is_invisible": invisible,
        "actor_color": list(color),
        "metallic": metallic,
        "roughness": roughness,
        "clearcoat": 0.0,
        "clearcoat_roughness": 0.1,
        "sheen": 0.0,
        "subsurface": 0.0,
        "transform": {
            "position": list(pos),
            "rotation": list(rot),
            "scale": list(scale),
        },
    }
    if light:
        a["light_type"] = light[0]
        a["light_color"] = list(light[1])
        a["light_intensity"] = light[2]
    return a


actors = []

# --- shell -------------------------------------------------------------------
actors.append(actor("Floor", "Square", (0.0, 0.0, 0.0), (41.0, 1.0, 41.0),
                    (0.16, 0.15, 0.14), roughness=0.9))
actors.append(actor("Ceiling", "Square", (0.0, WALL_H, 0.0), (41.0, 1.0, 41.0),
                    (0.09, 0.09, 0.10), roughness=0.95))

for name, cx, cz, sx, sz in WALLS:
    tone = (0.20, 0.20, 0.21) if name.startswith("Wall") else (0.17, 0.17, 0.18)
    actors.append(actor(name, "Cube", (cx, WALL_H * 0.5, cz), (sx, WALL_H, sz),
                        tone, roughness=0.88))

# --- beacons: the objectives, and most of the light in the building -----------
# The emitter is its own invisible actor rather than a component on the lamp: a
# point light sitting inside its own sphere is occluded by that sphere from every
# direction, which is exactly as dark as it sounds.
for name, bx, bz in BEACONS:
    actors.append(actor(name, "Cube", (bx, 0.7, bz), (0.5, 1.4, 0.5),
                        (0.35, 0.75, 0.95), roughness=0.3))
    actors.append(actor(name + "_Glow", "Sphere", (bx, 1.6, bz), (0.4, 0.4, 0.4),
                        (0.75, 0.95, 1.0), roughness=0.15))
    actors.append(actor(name + "_Light", "Cube", (bx, 2.15, bz), (0.1, 0.1, 0.1),
                        (1.0, 1.0, 1.0), invisible=True,
                        light=("PointLight", (0.45, 0.75, 1.0), 26.0)))

# --- the hub: where you start and where you leave from ------------------------
actors.append(actor("ExitPad", "Square", (0.0, 0.06, 0.0), (4.0, 1.0, 4.0),
                    (0.30, 0.22, 0.10), roughness=0.5))
actors.append(actor("ExitLamp", "Sphere", (0.0, 3.3, 0.0), (0.3, 0.3, 0.3),
                    (1.0, 0.7, 0.35), roughness=0.3))
actors.append(actor("ExitLamp_Light", "Cube", (0.0, 2.85, 0.0), (0.1, 0.1, 0.1),
                    (1.0, 1.0, 1.0), invisible=True,
                    light=("PointLight", (1.0, 0.55, 0.2), 14.0)))

# Four failing strip lights over the corridors. Enough to walk by, far enough
# apart that the space between them is genuinely dark.
for i, (lx, lz) in enumerate(STRIPS):
    actors.append(actor("Strip_%d" % i, "Cube", (lx, 3.75, lz), (1.6, 0.12, 1.6),
                        (0.55, 0.58, 0.62), roughness=0.4))
    actors.append(actor("Strip_%d_Light" % i, "Cube", (lx, 3.5, lz), (0.1, 0.1, 0.1),
                        (1.0, 1.0, 1.0), invisible=True,
                        light=("PointLight", (0.55, 0.60, 0.72), 4.0)))

# --- the thing that hunts you, and the game it carries ------------------------
stalker = actor("Stalker", "Cube", (-16.0, 1.0, -6.0), (0.75, 2.0, 0.75),
                (0.05, 0.04, 0.05), roughness=0.7)
stalker["script"] = {"type": "cminus", "path": SCRIPT_PATH}
actors.append(stalker)

# --- ambience -----------------------------------------------------------------
moon = actor("Moon", "Cube", (0.0, 12.0, 0.0), (1.0, 1.0, 1.0),
             (1.0, 1.0, 1.0), rot=(0.9, -0.6, 0.0),
             light=("DirectionalLight", (0.30, 0.38, 0.55), 0.06),
             invisible=True)
# Flat near-black background instead of a sky. Uniform ambient is what flattens a
# dark scene into grey: it lifts the shadows the lamps are supposed to carve out.
moon["sky_mode"] = 2
moon["void_color"] = [0.004, 0.006, 0.012]
moon["enable_3d_clouds"] = False
actors.append(moon)

actors.append(actor("Ambience", "Cube", (0.0, 2.0, 0.0), (1.0, 1.0, 1.0),
                    (1.0, 1.0, 1.0), light=("SkyLight", (0.10, 0.12, 0.18), 0.04),
                    invisible=True))

scene = {
    "actors": actors,
    # The scene names its own lighting environment. The engine's default is a
    # daytime HDRI, and its image-based ambient does not care that there is a
    # ceiling in the way - every surface in here came back washed out under it.
    "environment": {"hdri": "EngineContent/NightSky.hdr"},
}

out = os.path.join(HERE, "lithium_test.lithium")
with open(out, "w") as f:
    json.dump(scene, f, indent=4)
print("wrote %s (%d actors)" % (out, len(actors)))

# --- the table the script has to agree with -----------------------------------
print("\n// paste into the init block of lithium_test.cminus:")
print("    wall_count = %d;" % len(WALLS))
for i, (name, cx, cz, sx, sz) in enumerate(WALLS):
    print("    wx[%d] = %.2f; wz[%d] = %.2f; wsx[%d] = %.2f; wsz[%d] = %.2f;   // %s"
          % (i, cx, i, cz, i, sx * 0.5, i, sz * 0.5, name))
for i, (name, bx, bz) in enumerate(BEACONS):
    print("    bx[%d] = %.2f; bz[%d] = %.2f;   // %s" % (i, bx, i, bz, name))
