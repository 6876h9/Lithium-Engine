#include "world/lod_group_component.hpp"
#include "core/mesh_resource.hpp"
#include "core/resource_manager.hpp"

#include <algorithm>
#include <cmath>

LODGroupComponent::LODGroupComponent(Actor* owner, const std::string& name)
    : ActorComponent(owner, name) {}

LODGroupComponent::~LODGroupComponent() = default;

float LODGroupComponent::compute_screen_height(float world_radius, double distance, float tan_half_fov) {
    // The viewport is 2 * distance * tan(fov/2) world units tall at this depth, and
    // the sphere is 2 * radius tall, so the ratio is radius / (distance * tan).
    if (distance <= 1e-4 || tan_half_fov <= 1e-6f) return 1.0f;
    const float height = static_cast<float>(world_radius / (distance * tan_half_fov));
    return (height > 1.0f) ? 1.0f : height;
}

int LODGroupComponent::select_level(float screen_height, double distance) const {
    if (distance <= static_cast<double>(minimum_detail_distance)) return -1;

    if (cull_screen_height > 0.0f && screen_height < cull_screen_height) {
        return static_cast<int>(levels.size());
    }

    // levels[i].screen_height is the size at which level i starts being used, and
    // the list runs most detailed to least, so thresholds descend. Walk down while
    // the object is still smaller than the next threshold; the last one it fell
    // below is the level to draw. Staying above levels[0]'s threshold leaves the
    // result at -1, which is the actor's own full-detail mesh.
    int chosen = -1;
    for (int i = 0; i < static_cast<int>(levels.size()); ++i) {
        if (screen_height >= levels[i].screen_height) break;
        chosen = i;
    }
    return chosen;
}

std::shared_ptr<MeshResource> LODGroupComponent::resource_for_level(int level) {
    if (level < 0 || level >= static_cast<int>(levels.size())) return nullptr;

    LODLevel& entry = levels[level];
    if (entry.mesh_path.empty()) return nullptr;

    // Requested once, on the frame the level is first needed. A scene full of
    // multi-level groups would otherwise stream every reduction at load time, which
    // is most of the memory LOD exists to save.
    if (!entry.requested || !entry.resource) {
        entry.resource = ResourceManager::get().load_async<MeshResource>(entry.mesh_path);
        entry.requested = true;
    }
    return entry.resource;
}
