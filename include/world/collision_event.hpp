#pragma once

#include "core/math.hpp"

class Actor;

// What a collision handler is told about a contact.
//
// Reported once per body pair per frame, not once per contact point: Jolt calls back
// per sub-shape pair, so a box resting on a floor produces four callbacks for what
// gameplay considers a single collision.
struct CollisionInfo {
    // The other party. Null only when it was destroyed in the same frame the contact
    // ended, which is exactly when an Exit handler still needs to run.
    Actor* other = nullptr;
    // World-space contact point and normal. The normal points out of `other` toward
    // the actor being notified, so it is the direction to push away along.
    // Both are zero on Exit - by then there is no contact to describe.
    Vector3 point = { 0.0f, 0.0f, 0.0f };
    Vector3 normal = { 0.0f, 0.0f, 0.0f };
    // How fast the two were closing along the normal, in m/s, sampled before the
    // solver ran. This is the number to scale an impact sound or damage by; it is
    // zero for Stay and Exit.
    float approach_speed = 0.0f;
};
