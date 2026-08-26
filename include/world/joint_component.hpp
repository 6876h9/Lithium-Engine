#pragma once

#include "world/component.hpp"
#include "core/math.hpp"
#include <string>

namespace JPH { class TwoBodyConstraint; }

// A physics constraint tying this actor's rigid body to another one, or to the
// world.
//
// The component lives on the *moving* half of the pair - the door, not the frame;
// the wheel, not the chassis - and names the other half in connected_actor. That
// choice also fixes the motor's sign: a positive motor value drives the actor the
// joint is attached to.
//
// Nothing here works without a PhysicsAttribute on the same actor: a constraint
// acts between two rigid bodies, and an actor without one has no body to constrain.
class JointComponent : public ActorComponent {
public:
    // Constraint shapes. The numbering is part of the scene file format, so new
    // types append rather than reorder.
    enum JointType : int {
        // Welds the two bodies at their current relative pose. Useful for gluing
        // compound objects together and for breaking one apart later.
        Joint_Fixed      = 0,
        // Ball and socket: position is locked, rotation is completely free.
        Joint_Point      = 1,
        // One rotational degree of freedom about `axis`. Doors, levers, wheels.
        Joint_Hinge      = 2,
        // One translational degree of freedom along `axis`. Pistons, lifts, drawers.
        Joint_Slider     = 3,
        // Keeps two points between min_distance and max_distance apart. With a
        // spring this is a spring joint; without one, a rigid rod or a rope.
        Joint_Distance   = 4,
        // Position locked, rotation limited to a cone about `axis`. The cheap
        // ragdoll joint.
        Joint_Cone       = 5,
        // Cone limit plus an independent twist limit about `axis`. This is the
        // joint a shoulder or a hip actually needs.
        Joint_SwingTwist = 6,
        Joint_Count      = 7
    };

    static const char* joint_type_name(int type);
    // True when the type has a driveable motor, so the editor can grey the rest out.
    static bool joint_type_has_motor(int type);
    static bool joint_type_has_limits(int type);

    JointComponent(Actor* owner, const std::string& name);
    virtual ~JointComponent();

    virtual void begin_play() override;
    // Deliberately empty: joints are created and driven from Engine::update()
    // instead. Building a constraint calls PhysicsSystem::AddConstraint and locks
    // both bodies, neither of which is safe from the parallel actor tick. See
    // update_joint().
    virtual void tick(float delta_time) override {}

    // Creates the constraint once both bodies exist, then pushes the current motor
    // settings to it. Must be called on the logic thread, outside the physics step.
    void update_joint(float delta_time);

    // --- Connection ----------------------------------------------------------
    // Name of the actor holding the other body. Empty anchors this joint to the
    // world, which is what a door frame or a fixed pivot is.
    std::string connected_actor;

    // Pivot point, in this actor's local space. The connected body is attached at
    // the same world position, which is what makes a joint authored in place
    // "just work" without a second anchor to keep in sync.
    Vector3 anchor = { 0.0f, 0.0f, 0.0f };
    // Hinge/slider/cone/twist axis, in this actor's local space. Ignored by the
    // fixed, point and distance types, which have no preferred direction.
    Vector3 axis = { 0.0f, 1.0f, 0.0f };

    int joint_type = Joint_Hinge;

    // --- Limits --------------------------------------------------------------
    bool enable_limits = false;
    // Degrees for the hinge, metres for the slider. Ignored by the other types,
    // which carry their limits in the fields below.
    float limit_min = -45.0f;
    float limit_max = 45.0f;

    // Distance joint only, in metres. A negative value means "whatever the two
    // anchors are apart right now", which is how a rope authored in place keeps
    // the length it was drawn at.
    float min_distance = -1.0f;
    float max_distance = -1.0f;

    // Cone and swing-twist, in degrees. swing_angle is the half-angle of the cone
    // the axis may tilt within; the twist range is about the axis itself.
    float swing_angle = 45.0f;
    float twist_min = -45.0f;
    float twist_max = 45.0f;

    // --- Spring --------------------------------------------------------------
    // Softens the limit rather than stopping dead at it: the hinge and slider get a
    // springy end stop, and the distance joint becomes a spring joint. Frequency is
    // in Hz, damping is a ratio where 1 is critically damped.
    bool enable_spring = false;
    float spring_frequency = 2.0f;
    float spring_damping = 1.0f;

    // --- Motor ---------------------------------------------------------------
    // Hinge and slider only. The motor drives toward a velocity, in rad/s for the
    // hinge and m/s for the slider, limited by motor_max_force (N m, or N).
    bool enable_motor = false;
    float motor_target_velocity = 0.0f;
    float motor_max_force = 1000.0f;

    // Resistance applied when the motor is off: N m for the hinge, N for the
    // slider. This is what stops a door swinging forever once pushed.
    float friction = 0.0f;

    // --- Runtime -------------------------------------------------------------
    void set_motor_enabled(bool enabled);
    void set_motor_target(float target_velocity);
    // Hinge angle in degrees, or slider offset in metres. Zero for the types that
    // have no single scalar to report.
    float get_current_value() const;

    bool has_joint() const { return constraint != nullptr; }
    // Why the joint could not be created, for the editor to show. Empty when fine.
    const std::string& get_status() const { return status; }

private:
    // Returns false if a body it needs does not exist yet, in which case the caller
    // should try again on a later tick.
    bool try_create_joint();
    void destroy_joint();
    // Pushes enable_motor / motor_target_velocity onto a live constraint.
    void apply_motor();

    JPH::TwoBodyConstraint* constraint = nullptr;
    // Set when begin_play could not build the joint yet. Bodies are created by each
    // actor's own begin_play, so the actor a joint connects to is frequently not
    // built at the instant this one is.
    bool creation_pending = false;
    std::string status;
};
