#pragma once

#include "world/actor.hpp"

// Static SLR (Specific Light Ray): an authored, non-dynamic volumetric light shaft.
//
// Unlike the scene's directional light, an SLR does not illuminate surfaces - it is a
// participating-media volume that scatters light toward the camera, so it reads as a
// visible beam hanging in the air. Its extent is the actor's transform (a unit box
// scaled/rotated into place), which is what gives the beam its shape.
class StaticSLRActor : public Actor {
public:
    // Beam tint. The scattering accumulation is multiplied by this directly, so the
    // rendered air density matches the picked colour exactly rather than approximating it.
    Vector3 slr_color = { 0.55f, 0.75f, 1.0f };
    // Alpha acts as an overall density/opacity scale for the volume.
    float slr_alpha = 1.0f;

    // 0 = fully diffused edges that bleed softly into darkness,
    // 1 = mathematically hard edges and crisp corners with no blur at all.
    // Volume shape: 0 = Box, 1 = Cone (apex at the top, widening downward - a
    // spotlight shaft).
    int shape = 0;

    float sharpness = 0.75f;

    // Scattering strength (brightness of the beam).
    float intensity = 1.0f;

    // How quickly the beam dims along its length, away from the emitting end.
    float falloff = 1.6f;
    // How tightly scattering concentrates toward the beam's axis. 0 spreads energy
    // evenly across the width (which reads as fog); 1 gives a bright, narrow core.
    float core = 0.65f;

    StaticSLRActor(const std::string& name) : Actor(name) {
        // A unit box by default; scale the actor to shape the beam.
        get_actor_transform().scale = { 1.0f, 4.0f, 1.0f };
    }
};
