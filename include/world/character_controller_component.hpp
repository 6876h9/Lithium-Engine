#pragma once

#include "world/component.hpp"
#include "core/math.hpp"
#include <memory>
#include <string>

namespace JPH { class CharacterVirtual; }

// A walking character, built on Jolt's CharacterVirtual.
//
// CharacterVirtual rather than a dynamic rigid body because a capsule with a motor
// is not how players expect to move: it tips over, slides down ramps, bounces off
// steps and carries momentum through direction changes. CharacterVirtual instead
// sweeps a shape and resolves contacts directly, which is what gives it the
// stop-on-a-dime feel, stair stepping and slope limiting that gameplay needs.
//
// The trade-off is that other rigid bodies cannot see it, since it is not a body in
// the simulation. Pushing dynamic objects around is not supported here.
class CharacterControllerComponent : public SceneComponent {
public:
    CharacterControllerComponent(Actor* owner, const std::string& name);
    virtual ~CharacterControllerComponent();

    virtual void begin_play() override;
    // Deliberately empty: the character is stepped from Engine::update() instead.
    // Jolt's temp allocator is a non-thread-safe linear allocator and actor tick()
    // runs across a task graph, so stepping here would let two characters corrupt
    // it. See update_character().
    virtual void tick(float delta_time) override {}

    // Steps the character. Must be called on the logic thread, after the physics
    // step and before the camera is placed, so the view does not lag a frame behind.
    void update_character(float delta_time);

    // --- Shape ---------------------------------------------------------------
    // Total capsule height is 2 * (half_height + radius); the defaults give a
    // roughly 1.8m humanoid.
    float capsule_radius = 0.3f;
    float capsule_half_height = 0.6f;

    // Gameplay collision layer the character sweeps against. Its row in the layer
    // matrix decides what it can walk into.
    int collision_layer = 0;

    // --- Movement ------------------------------------------------------------
    float walk_speed = 5.0f;
    float sprint_multiplier = 1.8f;
    float jump_speed = 4.5f;
    float gravity_scale = 1.0f;
    // Degrees. Slopes steeper than this are walls: the character slides rather than
    // climbing, which is what stops players walking up near-vertical surfaces.
    float max_slope_angle = 45.0f;
    // Height of a step the character walks up without jumping. Zero disables it,
    // which makes even a kerb an impassable wall.
    float step_height = 0.4f;

    // --- Input ---------------------------------------------------------------
    // Drives the character from the MoveForward / MoveRight / Jump / Sprint input
    // actions. Turn off to drive it entirely from script or C++ via set_move_input().
    bool use_player_input = true;
    // Turns the actor's yaw with horizontal mouse movement. The active camera reads
    // the actor's rotation, so this is what makes it a first-person controller.
    bool mouse_look = true;
    float mouse_sensitivity = 0.0025f;

    // --- Scripted control ----------------------------------------------------
    // Movement intent for the coming frame, in the actor's local frame: +y is
    // forward, +x is right. Magnitude is clamped to 1. Consumed each update.
    void set_move_input(float right, float forward);
    void request_jump() { jump_requested = true; }

    bool is_grounded() const;
    Vector3 get_velocity() const;
    // Places the character directly, bypassing collision. For spawning and respawns.
    void teleport(const DVector3& position);

private:
    void sync_actor_transform();

    std::unique_ptr<JPH::CharacterVirtual> character;
    float input_right = 0.0f;
    float input_forward = 0.0f;
    bool jump_requested = false;
    bool sprinting = false;
    // Retained across frames so gravity accumulates while falling; CharacterVirtual
    // does not integrate it on its own.
    float vertical_velocity = 0.0f;
};
