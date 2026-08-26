#include "world/character_controller_component.hpp"
#include "world/actor.hpp"
#include "physics/physics_engine.hpp"
#include "core/input_map.hpp"
#include "world/ui_canvas_component.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

#include <algorithm>
#include <cmath>
#include <iostream>

CharacterControllerComponent::CharacterControllerComponent(Actor* owner, const std::string& name)
    : SceneComponent(owner, name) {
    // Actor::get_actor_transform() falls back to a single static Transform shared by
    // every rootless actor in the scene, so a character on a bare Actor would read
    // and write the same position as every other one. Claim the root only when the
    // actor has none - if a mesh is already root, writing the character's position
    // into that transform is exactly what makes the visible body follow it.
    if (owner && !owner->get_root_component()) {
        owner->set_root_component(this);
    }
}

// Out of line so the header can forward declare CharacterVirtual.
CharacterControllerComponent::~CharacterControllerComponent() = default;

void CharacterControllerComponent::begin_play() {
    character.reset();
    vertical_velocity = 0.0f;
    jump_requested = false;

    PhysicsEngine& physics = PhysicsEngine::get_instance();
    JPH::PhysicsSystem* system = physics.get_physics_system();
    if (!system || !physics.is_ready()) {
        std::cerr << "[CharacterController] " << owner->get_name()
                  << ": physics not initialised; character disabled." << std::endl;
        return;
    }

    const float radius = std::max(0.01f, capsule_radius);
    const float half_height = std::max(0.01f, capsule_half_height);

    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();

    // The capsule is lifted so the character's origin sits at its feet rather than
    // its middle. Without this an actor placed at ground level spawns half buried,
    // and its transform no longer means what the editor gizmo shows.
    settings->mShape = JPH::RotatedTranslatedShapeSettings(
                           JPH::Vec3(0, half_height + radius, 0),
                           JPH::Quat::sIdentity(),
                           new JPH::CapsuleShape(half_height, radius))
                           .Create().Get();

    settings->mMaxSlopeAngle = JPH::DegreesToRadians(std::clamp(max_slope_angle, 0.0f, 89.0f));
    // Contacts below the bottom of the capsule count as supporting the character.
    // Left at its default the whole capsule counts, so brushing a wall reads as
    // standing on it and the character can jump in mid-air off any surface.
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);

    const Transform& t = owner->get_actor_transform();
    character = std::unique_ptr<JPH::CharacterVirtual>(new JPH::CharacterVirtual(
        settings,
        JPH::RVec3(t.position.x, t.position.y, t.position.z),
        JPH::Quat::sIdentity(),
        system));
}

void CharacterControllerComponent::set_move_input(float right, float forward) {
    // Clamped as a vector, not per axis: otherwise holding two directions gives
    // sqrt(2) times the speed of holding one.
    float length = std::sqrt(right * right + forward * forward);
    if (length > 1.0f) {
        right /= length;
        forward /= length;
    }
    input_right = right;
    input_forward = forward;
}

bool CharacterControllerComponent::is_grounded() const {
    if (!character) return false;
    return character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

Vector3 CharacterControllerComponent::get_velocity() const {
    if (!character) return { 0.0f, 0.0f, 0.0f };
    JPH::Vec3 v = character->GetLinearVelocity();
    return { v.GetX(), v.GetY(), v.GetZ() };
}

void CharacterControllerComponent::teleport(const DVector3& position) {
    if (!character) return;
    character->SetPosition(JPH::RVec3(position.x, position.y, position.z));
    character->SetLinearVelocity(JPH::Vec3::sZero());
    vertical_velocity = 0.0f;
    sync_actor_transform();
}

void CharacterControllerComponent::sync_actor_transform() {
    if (!character) return;
    JPH::RVec3 p = character->GetPosition();
    Transform& t = owner->get_actor_transform();
    t.position = { static_cast<double>(p.GetX()), static_cast<double>(p.GetY()), static_cast<double>(p.GetZ()) };
}

void CharacterControllerComponent::update_character(float delta_time) {
    if (!character || delta_time <= 0.0f) return;

    PhysicsEngine& physics = PhysicsEngine::get_instance();
    JPH::PhysicsSystem* system = physics.get_physics_system();
    JPH::TempAllocator* allocator = physics.get_temp_allocator();
    if (!system || !allocator) return;

    Transform& transform = owner->get_actor_transform();

    // A focused UI text field owns the keyboard. Gameplay reads SDL's key state
    // directly rather than going through the UI, so without this check the player
    // walks off across the level while typing their name into a menu.
    if (use_player_input && !UICanvasComponent::any_keyboard_focus()) {
        InputMap& input = InputMap::get();
        set_move_input(input.axis("MoveRight"), input.axis("MoveForward"));
        if (input.pressed("Jump")) jump_requested = true;
        sprinting = input.held("Sprint");

        if (mouse_look) {
            // Yaw only. Pitch is the camera's business - pitching the capsule would
            // tilt the collider and let the player walk into the floor.
            transform.rotation.y -= input.mouse_dx() * mouse_sensitivity;
        }
    }

    // Movement intent is in the actor's local frame, so rotate it by the actor's yaw
    // to get world space. This is what makes W mean "the way I am facing".
    const float yaw = transform.rotation.y;
    const float sin_yaw = std::sin(yaw);
    const float cos_yaw = std::cos(yaw);
    // Matches the engine's camera basis, where -Z is forward at zero yaw.
    Vector3 forward_dir = { -sin_yaw, 0.0f, -cos_yaw };
    Vector3 right_dir   = {  cos_yaw, 0.0f, -sin_yaw };

    float speed = walk_speed * (sprinting ? std::max(1.0f, sprint_multiplier) : 1.0f);
    JPH::Vec3 horizontal(
        (forward_dir.x * input_forward + right_dir.x * input_right) * speed,
        0.0f,
        (forward_dir.z * input_forward + right_dir.z * input_right) * speed);

    const bool on_ground = character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;

    if (on_ground && vertical_velocity <= 0.0f) {
        // A small downward bias rather than zero: it keeps the character pressed
        // into the floor so StickToFloor can hold it on descending slopes instead of
        // letting it launch off every crest.
        vertical_velocity = -0.5f;
        if (jump_requested) {
            vertical_velocity = jump_speed;
        }
    } else {
        vertical_velocity += physics.get_gravity_y() * gravity_scale * delta_time;
    }
    jump_requested = false;

    // Inherit the motion of whatever is underfoot, so a moving platform carries the
    // character instead of sliding out from under them.
    JPH::Vec3 ground_velocity = on_ground ? character->GetGroundVelocity() : JPH::Vec3::sZero();
    character->SetLinearVelocity(JPH::Vec3(horizontal.GetX() + ground_velocity.GetX(),
                                           vertical_velocity,
                                           horizontal.GetZ() + ground_velocity.GetZ()));

    JPH::CharacterVirtual::ExtendedUpdateSettings update_settings;
    update_settings.mWalkStairsStepUp = JPH::Vec3(0.0f, std::max(0.0f, step_height), 0.0f);

    character->ExtendedUpdate(delta_time,
                              JPH::Vec3(0.0f, physics.get_gravity_y() * gravity_scale, 0.0f),
                              update_settings,
                              // A character is always a moving body, so its object
                              // layer is the moving half of its gameplay layer.
                              system->GetDefaultBroadPhaseLayerFilter(
                                  PhysicsEngine::make_object_layer(collision_layer, true)),
                              system->GetDefaultLayerFilter(
                                  PhysicsEngine::make_object_layer(collision_layer, true)),
                              {}, {}, *allocator);

    // Landing and head-bumps both have to clear the retained vertical speed, or the
    // character keeps accumulating downward velocity while standing still and shoots
    // through the floor the moment it steps off a ledge.
    JPH::Vec3 resolved = character->GetLinearVelocity();
    if (character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround) {
        if (vertical_velocity < 0.0f) vertical_velocity = 0.0f;
    } else {
        vertical_velocity = resolved.GetY();
    }

    sync_actor_transform();

    // Consumed, so a script that stops calling set_move_input() stops the character
    // rather than leaving it walking into a wall forever.
    if (!use_player_input) {
        input_right = 0.0f;
        input_forward = 0.0f;
    }
}
