#pragma once

#include "world/component.hpp"
#include "world/actor.hpp"
#include "core/math.hpp"
#include <cstdint>

class PhysicsAttribute : public ActorComponent {
public:
    // Collider shapes. The numbering is part of the scene file format - Box and
    // Sphere were 0 and 1 before the others existed, so new shapes append rather
    // than reorder, or every saved scene would silently change collider.
    enum ColliderType : int {
        Collider_Box        = 0,
        Collider_Sphere     = 1,
        Collider_Capsule    = 2,
        Collider_Cylinder   = 3,
        // Built from the owner's mesh. ConvexHull wraps the vertices in their convex
        // envelope and can move; Mesh keeps every triangle and, per Jolt, can only
        // ever be static - a concave shape has no meaningful inertia tensor.
        Collider_ConvexHull = 4,
        Collider_Mesh       = 5,
        Collider_Count      = 6
    };

    static const char* collider_type_name(int type);

    PhysicsAttribute(Actor* owner, const std::string& name);
    virtual ~PhysicsAttribute();

    virtual void begin_play() override;
    virtual void tick(float delta_time) override;

    // Jolt Physics Properties
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.0f;
    bool simulate_gravity = true;

    int collider_type = Collider_Box;
    Vector3 box_half_extents = {0.5f, 0.5f, 0.5f};
    float sphere_radius = 0.5f;
    // Capsule and cylinder are described by a radius and the half height of the
    // straight middle section, matching Jolt's own parameterisation. A capsule's
    // total height is therefore 2 * (half_height + radius).
    float capsule_radius = 0.35f;
    float capsule_half_height = 0.5f;
    float cylinder_radius = 0.5f;
    float cylinder_half_height = 0.5f;

    // A trigger reports overlap without pushing anything: the body becomes a Jolt
    // sensor, so it still generates contact callbacks but no collision response.
    // Contacts with it arrive as on_trigger_* instead of on_collision_*.
    bool is_trigger = false;

    // Which gameplay collision layer this body belongs to. The layer matrix in
    // Settings > Collision Layers decides which other layers it interacts with.
    int collision_layer = 0;

    bool has_body() const { return body_id != 0xFFFFFFFFu; }
    // Jolt's id for this body, or 0xFFFFFFFF when none exists. Needed by anything
    // that has to name the body to Jolt without going through this component -
    // constraints, which act between two bodies rather than on one.
    uint32_t get_body_id() const { return body_id; }
    // Why the body could not be created, for the editor to show. Empty when fine.
    const std::string& get_status() const { return status; }

private:
    // Returns false if the shape needs mesh data that has not finished loading, in
    // which case the caller should try again on a later tick.
    bool try_create_body();
    void destroy_body();

    uint32_t body_id = 0xFFFFFFFF; // JPH::BodyID::cInvalidBodyID
    // Set when begin_play could not build the body yet because the owner's mesh was
    // still streaming in. Meshes load asynchronously, so a convex hull or triangle
    // collider is frequently not buildable at the instant Play is pressed.
    bool creation_pending = false;
    std::string status;
};
