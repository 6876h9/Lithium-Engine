#include "world/nav_agent_component.hpp"
#include "world/actor.hpp"
#include "world/character_controller_component.hpp"
#include "navigation/navmesh.hpp"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

// Shortest signed angle from `from` to `to`, in radians. Without the wrap an agent
// facing just west of north and asked to turn to just east of north takes the long
// way round - a full circle to cover two degrees.
float shortest_angle(float from, float to) {
    float difference = to - from;
    while (difference > kPi) difference -= 2.0f * kPi;
    while (difference < -kPi) difference += 2.0f * kPi;
    return difference;
}

} // namespace

NavAgentComponent::NavAgentComponent(Actor* owner, const std::string& name)
    : ActorComponent(owner, name) {}

NavAgentComponent::~NavAgentComponent() = default;

void NavAgentComponent::stop() {
    path.clear();
    waypoint = 0;
    arrived = true;
    // The controller keeps whatever intent it was last given, so an agent that stops
    // without clearing it would keep walking.
    if (auto* character = owner ? owner->get_component<CharacterControllerComponent>() : nullptr) {
        character->set_move_input(0.0f, 0.0f);
    }
}

bool NavAgentComponent::set_destination(const DVector3& target) {
    destination = target;
    time_since_repath = 0.0f;
    if (!recompute_path()) {
        stop();
        return false;
    }
    arrived = false;
    return true;
}

bool NavAgentComponent::recompute_path() {
    status.clear();
    NavMesh& navmesh = NavMesh::get();
    if (!navmesh.is_built()) {
        status = "No navmesh has been built for this scene.";
        return false;
    }
    if (!owner) return false;

    std::vector<DVector3> new_path;
    if (!navmesh.find_path(owner->get_actor_transform().position, destination, new_path)) {
        status = "No route to that destination.";
        return false;
    }

    path = std::move(new_path);
    // The first point is where the agent already is, so walking to it is a no-op
    // that would only make the agent hesitate for a frame.
    waypoint = (path.size() > 1) ? 1 : 0;
    return true;
}

float NavAgentComponent::remaining_distance() const {
    if (path.empty() || waypoint >= path.size() || !owner) return 0.0f;

    const DVector3& position = owner->get_actor_transform().position;
    double total = (path[waypoint] - position).length();
    for (size_t i = waypoint; i + 1 < path.size(); ++i) {
        total += (path[i + 1] - path[i]).length();
    }
    return static_cast<float>(total);
}

void NavAgentComponent::apply_movement(const Vector3& world_direction, float delta_time, double target_y) {
    Transform& transform = owner->get_actor_transform();

    if (rotate_to_face && (std::abs(world_direction.x) > 1e-4f || std::abs(world_direction.z) > 1e-4f)) {
        // The engine's convention is -Z forward at zero yaw, which is what puts the
        // negations in the atan2 arguments.
        const float desired_yaw = std::atan2(-world_direction.x, -world_direction.z);
        if (angular_speed <= 0.0f) {
            transform.rotation.y = desired_yaw;
        } else {
            const float difference = shortest_angle(transform.rotation.y, desired_yaw);
            const float step = angular_speed * kDegToRad * delta_time;
            transform.rotation.y += (std::abs(difference) <= step)
                                        ? difference
                                        : ((difference > 0.0f) ? step : -step);
        }
    }

    if (auto* character = owner->get_component<CharacterControllerComponent>()) {
        // The controller takes intent in the actor's own frame, so the world
        // direction has to be projected onto the actor's forward and right axes.
        const float yaw = transform.rotation.y;
        const float sin_yaw = std::sin(yaw);
        const float cos_yaw = std::cos(yaw);
        const Vector3 forward_dir = { -sin_yaw, 0.0f, -cos_yaw };
        const Vector3 right_dir   = {  cos_yaw, 0.0f, -sin_yaw };

        const float forward = Vector3::dot(world_direction, forward_dir);
        const float right   = Vector3::dot(world_direction, right_dir);
        character->set_move_input(right, forward);
        // Height is the controller's business: it is standing on the collision
        // world, not on the navmesh's idea of where the floor is.
        return;
    }

    // No controller: move the transform along the path directly. The height is taken
    // from the path rather than integrated, because there is nothing here to fall.
    transform.position.x += static_cast<double>(world_direction.x) * speed * delta_time;
    transform.position.z += static_cast<double>(world_direction.z) * speed * delta_time;

    // Eased rather than snapped, so a step in the path does not teleport the actor
    // vertically. The rate matches the horizontal speed, which keeps a slope looking
    // like a slope.
    const double height_step = static_cast<double>(speed) * delta_time;
    const double height_difference = target_y - transform.position.y;
    if (std::abs(height_difference) <= height_step) {
        transform.position.y = target_y;
    } else {
        transform.position.y += (height_difference > 0.0 ? height_step : -height_step);
    }
}

void NavAgentComponent::update_agent(float delta_time) {
    if (!owner || arrived || path.empty()) return;

    if (auto_repath && repath_interval > 0.0f) {
        time_since_repath += delta_time;
        if (time_since_repath >= repath_interval) {
            time_since_repath = 0.0f;
            // A failed recompute leaves the previous path in place: losing the route
            // for one frame - a navmesh mid-rebuild, a target that stepped off the
            // surface - should not make a walking character stop dead.
            recompute_path();
            if (path.empty()) return;
        }
    }

    const DVector3& position = owner->get_actor_transform().position;

    // Consume every waypoint already reached. More than one can fall inside the
    // tolerance at once when the path doubles back or the agent is moving fast.
    while (waypoint < path.size()) {
        const DVector3 offset = path[waypoint] - position;
        const double horizontal = std::sqrt(offset.x * offset.x + offset.z * offset.z);
        const bool is_final = (waypoint + 1 == path.size());
        const double threshold = is_final ? stopping_distance : waypoint_tolerance;
        if (horizontal > threshold) break;
        if (is_final) {
            stop();
            arrived = true;
            return;
        }
        ++waypoint;
    }

    if (waypoint >= path.size()) {
        stop();
        arrived = true;
        return;
    }

    const DVector3 offset = path[waypoint] - position;
    Vector3 direction = { static_cast<float>(offset.x), 0.0f, static_cast<float>(offset.z) };
    const float length = direction.length();
    if (length < 1e-5f) {
        // Directly above or below the waypoint: nothing to steer towards, so take
        // the next one rather than dividing by zero.
        ++waypoint;
        return;
    }
    direction = direction / length;

    apply_movement(direction, delta_time, path[waypoint].y);
}
