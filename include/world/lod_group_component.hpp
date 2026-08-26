#pragma once

#include "world/component.hpp"
#include "core/math.hpp"
#include <memory>
#include <string>
#include <vector>

class MeshResource;

// Level-of-detail switching for the actor's static mesh.
//
// Each level names a cheaper mesh and the screen size below which it takes over.
// "Screen size" is the fraction of the viewport's height the object's bounding
// sphere covers, which is the only measure that behaves the same at every
// resolution and field of view - a distance threshold in metres would switch at the
// wrong moment the instant anyone changed the FOV.
//
// Level 0 is the actor's own mesh and is not listed here; the entries below are the
// reductions, ordered from most to least detailed.
class LODGroupComponent : public ActorComponent {
public:
    struct LODLevel {
        // Mesh drawn at this level. Empty means "draw nothing", which is how a
        // level is used purely as a cull threshold.
        std::string mesh_path;
        // Fraction of viewport height, in (0, 1]. The level is chosen when the
        // object's on-screen height drops below the previous level's threshold and
        // is still at or above this one's.
        float screen_height = 0.25f;

        // Resolved lazily the first time the level is selected, so a scene with
        // eight LOD levels per actor does not stream every one of them at load.
        std::shared_ptr<MeshResource> resource;
        bool requested = false;
    };

    LODGroupComponent(Actor* owner, const std::string& name);
    virtual ~LODGroupComponent();

    // Deliberately empty: selection happens where the render command is built, on
    // the logic thread, because that is the only place the camera position for the
    // frame being submitted is known.
    virtual void tick(float delta_time) override {}

    // Distance below which the highest-detail mesh is always used regardless of
    // screen size. Stops a huge object - a terrain chunk, a building - from
    // dropping to its lowest LOD just because the camera is inside it.
    float minimum_detail_distance = 0.0f;

    // Below this screen height the object is not drawn at all. Zero disables it.
    // This is what makes a forest affordable: distant trees stop costing anything
    // rather than costing a little each.
    float cull_screen_height = 0.0f;

    std::vector<LODLevel> levels;

    // Chosen level for this frame: -1 means the actor's own mesh, >= 0 indexes
    // `levels`, and levels.size() means "culled". `screen_height` is the object's
    // on-screen height as a fraction of the viewport.
    int select_level(float screen_height, double distance) const;

    // Screen height of a sphere of `world_radius` at `distance`, as a fraction of
    // viewport height. tan_half_fov is the tangent of half the vertical field of
    // view, which is what turns a world size into a screen fraction.
    static float compute_screen_height(float world_radius, double distance, float tan_half_fov);

    // The mesh for a level, streamed in on first use. Null for level -1 (the
    // actor's own mesh), for a culled level, or while the asset is still loading.
    std::shared_ptr<MeshResource> resource_for_level(int level);

    // Level chosen on the most recent frame, for the editor to display. Not used by
    // selection itself, which is stateless.
    int get_last_selected_level() const { return last_selected_level; }
    void set_last_selected_level(int level) { last_selected_level = level; }

private:
    int last_selected_level = -1;
};
