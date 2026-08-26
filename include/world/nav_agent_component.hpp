#pragma once

#include "world/component.hpp"
#include "core/math.hpp"
#include <string>
#include <vector>

// Walks an actor along a path found through the scene's navmesh.
//
// Two ways of moving are supported and the right one is chosen automatically. An
// actor with a character controller is driven through it, so the agent collides,
// steps up kerbs and falls under gravity like a player would. An actor without one
// has its transform moved directly, which is what a floating or purely decorative
// mover wants and costs nothing.
class NavAgentComponent : public ActorComponent {
public:
    NavAgentComponent(Actor* owner, const std::string& name);
    virtual ~NavAgentComponent();

    // Deliberately empty: the agent is stepped from Engine::update() instead. It may
    // drive a character controller, whose collision queries run through Jolt's
    // non-thread-safe temp allocator, and the actor tick runs across a task graph.
    virtual void tick(float delta_time) override {}

    // Steps the agent. Must be called on the logic thread, after the physics step.
    void update_agent(float delta_time);

    // --- Movement ------------------------------------------------------------
    float speed = 3.5f;
    // Degrees per second the actor turns to face where it is going. Zero snaps.
    float angular_speed = 540.0f;
    // How close to the destination counts as arrived. Too small and the agent
    // oscillates around the last waypoint forever.
    float stopping_distance = 0.4f;
    // How close to an intermediate waypoint counts as reaching it.
    float waypoint_tolerance = 0.45f;
    // Turns the actor to face its direction of travel. Off for something that
    // should keep its own orientation - a turret, a camera rig.
    bool rotate_to_face = true;

    // --- Repathing -----------------------------------------------------------
    // Recomputes the route periodically while moving, so an agent chasing something
    // that keeps calling set_destination does not have to be told to recalculate,
    // and one whose route was invalidated by a navmesh rebuild recovers on its own.
    bool auto_repath = true;
    float repath_interval = 0.5f;

    // --- Control -------------------------------------------------------------
    // Finds a route to `target` and starts following it. Returns false if either end
    // is off the navigable surface or nothing connects them, leaving the agent
    // stopped rather than walking at a wall.
    bool set_destination(const DVector3& target);
    void stop();

    bool has_path() const { return !path.empty(); }
    bool is_moving() const { return !path.empty() && !arrived; }
    bool has_arrived() const { return arrived; }
    // Distance still to walk along the remaining path, in metres. Zero with no path.
    float remaining_distance() const;
    const DVector3& get_destination() const { return destination; }
    const std::vector<DVector3>& get_path() const { return path; }
    // Why the last set_destination failed, for the editor to show. Empty when fine.
    const std::string& get_status() const { return status; }

private:
    // Recomputes the route to the stored destination, keeping the agent moving if it
    // fails - a momentary failure should not make a walking character stop dead.
    bool recompute_path();
    // Feeds a world-space direction to the character controller, or moves the
    // transform directly when there is no controller.
    void apply_movement(const Vector3& world_direction, float delta_time, double target_y);

    std::vector<DVector3> path;
    // Index of the waypoint currently being walked towards.
    size_t waypoint = 0;
    DVector3 destination = { 0.0, 0.0, 0.0 };
    bool arrived = true;
    float time_since_repath = 0.0f;
    std::string status;
};
