#include "world/joint_component.hpp"
#include "world/physics_attribute.hpp"
#include "physics/physics_engine.hpp"
#include "core/engine.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>

#include <cmath>
#include <iostream>

namespace {

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;

// Any unit vector at right angles to `axis`. Jolt needs a second axis to pin down
// the constraint frame's roll; which one it is does not matter as long as it is
// perpendicular and the same for both bodies. Choosing the reference axis by the
// smallest component keeps the cross product well conditioned - crossing with a
// nearly parallel vector gives a near-zero result that normalises into garbage.
JPH::Vec3 perpendicular_to(JPH::Vec3Arg axis) {
    const float ax = std::abs(axis.GetX());
    const float ay = std::abs(axis.GetY());
    const float az = std::abs(axis.GetZ());
    JPH::Vec3 reference = JPH::Vec3::sAxisX();
    if (ay <= ax && ay <= az) reference = JPH::Vec3::sAxisY();
    else if (az <= ax && az <= ay) reference = JPH::Vec3::sAxisZ();
    return axis.Cross(reference).Normalized();
}

JPH::SpringSettings make_spring(bool enabled, float frequency, float damping) {
    JPH::SpringSettings spring;
    if (enabled) {
        spring.mMode = JPH::ESpringMode::FrequencyAndDamping;
        spring.mFrequency = std::max(0.0f, frequency);
        spring.mDamping = std::max(0.0f, damping);
    }
    return spring;
}

// The rigid body backing an actor, or the invalid id if it has none.
uint32_t body_of(Actor* actor) {
    if (!actor) return JPH::BodyID::cInvalidBodyID;
    auto* physics = actor->get_component<PhysicsAttribute>();
    return physics ? physics->get_body_id() : JPH::BodyID::cInvalidBodyID;
}

Actor* find_actor_by_name(const std::string& name) {
    if (!g_engine || name.empty()) return nullptr;
    for (auto& actor : g_engine->get_actors()) {
        if (actor && actor->get_name() == name) return actor.get();
    }
    return nullptr;
}

} // namespace

const char* JointComponent::joint_type_name(int type) {
    switch (type) {
        case Joint_Fixed:      return "Fixed";
        case Joint_Point:      return "Point (ball socket)";
        case Joint_Hinge:      return "Hinge";
        case Joint_Slider:     return "Slider";
        case Joint_Distance:   return "Distance / spring";
        case Joint_Cone:       return "Cone";
        case Joint_SwingTwist: return "Swing-twist (ragdoll)";
        default:               return "Unknown";
    }
}

bool JointComponent::joint_type_has_motor(int type) {
    return type == Joint_Hinge || type == Joint_Slider;
}

bool JointComponent::joint_type_has_limits(int type) {
    return type == Joint_Hinge || type == Joint_Slider;
}

JointComponent::JointComponent(Actor* owner, const std::string& name)
    : ActorComponent(owner, name) {}

JointComponent::~JointComponent() {
    destroy_joint();
}

void JointComponent::destroy_joint() {
    if (!constraint) return;
    if (JPH::PhysicsSystem* system = PhysicsEngine::get_instance().get_physics_system()) {
        system->RemoveConstraint(constraint);
    }
    // Balances the AddRef taken in try_create_joint. Jolt's constraints are
    // reference counted and Create() hands back one at a count of zero, so without
    // an explicit reference here the constraint would be freed the moment the
    // manager let go of it - or leak if it never did.
    constraint->Release();
    constraint = nullptr;
}

void JointComponent::begin_play() {
    // Rebuild from scratch: Play can be pressed more than once, and the previous
    // run's constraint still refers to bodies that no longer exist.
    destroy_joint();
    status.clear();
    creation_pending = !try_create_joint();
}

bool JointComponent::try_create_joint() {
    JPH::PhysicsSystem* system = PhysicsEngine::get_instance().get_physics_system();
    if (!system) {
        status = "Physics engine not initialised.";
        return true; // Nothing to wait for; retrying will not help.
    }

    const uint32_t own_body = body_of(owner);
    if (own_body == JPH::BodyID::cInvalidBodyID) {
        // The owner's own PhysicsAttribute may still be waiting on a streaming mesh
        // for its collider, so this is a "try again", not a failure - unless there
        // is no PhysicsAttribute at all, which never resolves.
        if (!owner || !owner->get_component<PhysicsAttribute>()) {
            status = "Needs a Physics Attribute on this actor.";
            return true;
        }
        status = "Waiting for this actor's rigid body...";
        return false;
    }

    Actor* other = nullptr;
    uint32_t other_body = JPH::BodyID::cInvalidBodyID;
    const bool to_world = connected_actor.empty();
    if (!to_world) {
        other = find_actor_by_name(connected_actor);
        if (!other) {
            status = "No actor named '" + connected_actor + "' in the scene.";
            return true;
        }
        if (other == owner) {
            status = "A joint cannot connect an actor to itself.";
            return true;
        }
        other_body = body_of(other);
        if (other_body == JPH::BodyID::cInvalidBodyID) {
            if (!other->get_component<PhysicsAttribute>()) {
                status = "'" + connected_actor + "' has no Physics Attribute.";
                return true;
            }
            status = "Waiting for '" + connected_actor + "' to build its rigid body...";
            return false;
        }
    }

    // World-space anchor and axis. Authoring them in the actor's local frame is what
    // makes a hinge stay on the correct edge of a door when the door is moved.
    const Transform& transform = owner->get_actor_transform();
    const Matrix4x4 model = transform.get_matrix();
    const Vector3 world_anchor = model * anchor;

    // The axis is a direction, so it takes the rotation but not the translation.
    Matrix4x4 rotation_only = model;
    rotation_only.m[12] = rotation_only.m[13] = rotation_only.m[14] = 0.0f;
    Vector3 world_axis_v = rotation_only * axis;
    if (world_axis_v.length() < 1e-6f) world_axis_v = { 0.0f, 1.0f, 0.0f };
    world_axis_v = world_axis_v.normalized();

    const JPH::RVec3 point(world_anchor.x, world_anchor.y, world_anchor.z);
    const JPH::Vec3 world_axis(world_axis_v.x, world_axis_v.y, world_axis_v.z);
    const JPH::Vec3 world_normal = perpendicular_to(world_axis);

    // Body 1 is the anchor, body 2 is this actor. Fixing that order is what gives
    // the motor an intuitive sign: driving the joint moves the actor it is on.
    JPH::Body* body1 = nullptr;
    JPH::Body* body2 = nullptr;

    // NoLock rather than the locking interface: this runs between physics steps on
    // the logic thread, so nothing else is touching these bodies, and taking two
    // locks at once is exactly the pattern that deadlocks if any other caller ever
    // takes them in the other order.
    const JPH::BodyLockInterface& lock_interface = system->GetBodyLockInterfaceNoLock();
    JPH::BodyLockWrite own_lock(lock_interface, JPH::BodyID(own_body));
    if (!own_lock.Succeeded()) {
        status = "This actor's rigid body is no longer valid.";
        return true;
    }
    body2 = &own_lock.GetBody();

    JPH::BodyLockWrite other_lock(lock_interface, to_world ? JPH::BodyID() : JPH::BodyID(other_body));
    if (to_world) {
        body1 = &JPH::Body::sFixedToWorld;
    } else {
        if (!other_lock.Succeeded()) {
            status = "'" + connected_actor + "' rigid body is no longer valid.";
            return true;
        }
        body1 = &other_lock.GetBody();
    }

    // A constraint needs something that can actually move. Two static bodies (or one
    // static body anchored to the world) produce a constraint that silently does
    // nothing, which reads as a broken joint rather than as a configuration mistake.
    if (!body1->IsDynamic() && !body2->IsDynamic()) {
        status = "Both ends are static; give one a non-zero mass or this joint does nothing.";
    }

    JPH::TwoBodyConstraint* created = nullptr;

    switch (joint_type) {
        case Joint_Fixed: {
            JPH::FixedConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = settings.mPoint2 = point;
            // A common world frame for both bodies, which locks in whatever relative
            // orientation they currently have.
            settings.mAxisX1 = settings.mAxisX2 = JPH::Vec3::sAxisX();
            settings.mAxisY1 = settings.mAxisY2 = JPH::Vec3::sAxisY();
            created = settings.Create(*body1, *body2);
            break;
        }

        case Joint_Point: {
            JPH::PointConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = settings.mPoint2 = point;
            created = settings.Create(*body1, *body2);
            break;
        }

        case Joint_Hinge: {
            JPH::HingeConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = settings.mPoint2 = point;
            settings.mHingeAxis1 = settings.mHingeAxis2 = world_axis;
            settings.mNormalAxis1 = settings.mNormalAxis2 = world_normal;
            if (enable_limits) {
                // Jolt requires min <= max and both within [-pi, pi].
                float lo = std::min(limit_min, limit_max) * kDegToRad;
                float hi = std::max(limit_min, limit_max) * kDegToRad;
                settings.mLimitsMin = std::max(-JPH::JPH_PI, lo);
                settings.mLimitsMax = std::min(JPH::JPH_PI, hi);
                settings.mLimitsSpringSettings = make_spring(enable_spring, spring_frequency, spring_damping);
            }
            settings.mMaxFrictionTorque = std::max(0.0f, friction);
            settings.mMotorSettings.SetTorqueLimit(std::max(0.0f, motor_max_force));
            created = settings.Create(*body1, *body2);
            break;
        }

        case Joint_Slider: {
            JPH::SliderConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = settings.mPoint2 = point;
            settings.mSliderAxis1 = settings.mSliderAxis2 = world_axis;
            settings.mNormalAxis1 = settings.mNormalAxis2 = world_normal;
            if (enable_limits) {
                settings.mLimitsMin = std::min(limit_min, limit_max);
                settings.mLimitsMax = std::max(limit_min, limit_max);
                settings.mLimitsSpringSettings = make_spring(enable_spring, spring_frequency, spring_damping);
            }
            settings.mMaxFrictionForce = std::max(0.0f, friction);
            settings.mMotorSettings.SetForceLimit(std::max(0.0f, motor_max_force));
            created = settings.Create(*body1, *body2);
            break;
        }

        case Joint_Distance: {
            JPH::DistanceConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = settings.mPoint2 = point;
            // Both anchors coincide, so "keep the current distance" would mean zero.
            // A negative authored value therefore means "leave it to Jolt", which
            // measures the real separation of the two bodies for itself.
            settings.mMinDistance = min_distance;
            settings.mMaxDistance = max_distance;
            if (settings.mMinDistance >= 0.0f && settings.mMaxDistance >= 0.0f &&
                settings.mMinDistance > settings.mMaxDistance) {
                std::swap(settings.mMinDistance, settings.mMaxDistance);
            }
            settings.mLimitsSpringSettings = make_spring(enable_spring, spring_frequency, spring_damping);
            created = settings.Create(*body1, *body2);
            break;
        }

        case Joint_Cone: {
            JPH::ConeConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = settings.mPoint2 = point;
            settings.mTwistAxis1 = settings.mTwistAxis2 = world_axis;
            settings.mHalfConeAngle = std::max(0.0f, std::min(180.0f, swing_angle)) * kDegToRad;
            created = settings.Create(*body1, *body2);
            break;
        }

        case Joint_SwingTwist: {
            JPH::SwingTwistConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPosition1 = settings.mPosition2 = point;
            settings.mTwistAxis1 = settings.mTwistAxis2 = world_axis;
            settings.mPlaneAxis1 = settings.mPlaneAxis2 = world_normal;
            const float half_cone = std::max(0.0f, std::min(180.0f, swing_angle)) * kDegToRad;
            settings.mNormalHalfConeAngle = half_cone;
            settings.mPlaneHalfConeAngle = half_cone;
            settings.mTwistMinAngle = std::max(-JPH::JPH_PI, std::min(twist_min, twist_max) * kDegToRad);
            settings.mTwistMaxAngle = std::min(JPH::JPH_PI, std::max(twist_min, twist_max) * kDegToRad);
            settings.mMaxFrictionTorque = std::max(0.0f, friction);
            created = settings.Create(*body1, *body2);
            break;
        }

        default:
            status = "Unknown joint type.";
            return true;
    }

    if (!created) {
        status = std::string(joint_type_name(joint_type)) + " constraint could not be created.";
        std::cerr << "[Joint] " << owner->get_name() << ": " << status << std::endl;
        return true;
    }

    created->AddRef();
    system->AddConstraint(created);
    constraint = created;
    apply_motor();
    return true;
}

void JointComponent::apply_motor() {
    if (!constraint) return;

    if (joint_type == Joint_Hinge) {
        auto* hinge = static_cast<JPH::HingeConstraint*>(constraint);
        // The torque limit lives in the motor settings, and setting a motor state
        // with an invalid limit trips a Jolt assert - so push it before the state.
        hinge->GetMotorSettings().SetTorqueLimit(std::max(0.0f, motor_max_force));
        hinge->SetMotorState(enable_motor ? JPH::EMotorState::Velocity : JPH::EMotorState::Off);
        hinge->SetTargetAngularVelocity(motor_target_velocity);
    } else if (joint_type == Joint_Slider) {
        auto* slider = static_cast<JPH::SliderConstraint*>(constraint);
        slider->GetMotorSettings().SetForceLimit(std::max(0.0f, motor_max_force));
        slider->SetMotorState(enable_motor ? JPH::EMotorState::Velocity : JPH::EMotorState::Off);
        slider->SetTargetVelocity(motor_target_velocity);
    }
}

void JointComponent::update_joint(float delta_time) {
    if (creation_pending) {
        creation_pending = !try_create_joint();
        return;
    }
    if (!constraint) return;

    // Cheap enough to push unconditionally, and it means a script or the editor
    // changing a motor value takes effect on the next step without needing a
    // dirty flag that a script setting the field directly would never set.
    apply_motor();
}

void JointComponent::set_motor_enabled(bool enabled) {
    enable_motor = enabled;
    apply_motor();
}

void JointComponent::set_motor_target(float target_velocity) {
    motor_target_velocity = target_velocity;
    apply_motor();
}

float JointComponent::get_current_value() const {
    if (!constraint) return 0.0f;
    if (joint_type == Joint_Hinge) {
        return static_cast<JPH::HingeConstraint*>(constraint)->GetCurrentAngle() * kRadToDeg;
    }
    if (joint_type == Joint_Slider) {
        return static_cast<JPH::SliderConstraint*>(constraint)->GetCurrentPosition();
    }
    return 0.0f;
}
