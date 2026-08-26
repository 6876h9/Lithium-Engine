#pragma once

#include "world/component.hpp"
#include "world/static_mesh_component.hpp"  // Vertex, shared with every other mesh
#include "core/math.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class MeshResource;
class TextureResource;

// A sculptable heightmap terrain.
//
// The surface is a regular grid of `resolution` x `resolution` height samples
// spanning `world_size` metres, centred on the component's transform. That is the
// one representation that makes all four of the things a terrain has to do cheap:
// sculpting is a write to an array, collision is Jolt's height field shape
// directly, texture blending is a lookup into a splat map at the same resolution,
// and scattering foliage is a walk over the same grid.
//
// The mesh uses the engine's ordinary Vertex layout, so the existing shadow depth
// pass draws it with no special case. Only the material differs: terrain has four
// tiling layers blended by a splat map instead of one diffuse texture.
class TerrainComponent : public SceneComponent {
public:
    // Four layers is what fits in one RGBA splat texel. More would mean a second
    // texture and a second set of samplers for a case terrain rarely needs.
    static constexpr int kLayerCount = 4;
    // Jolt's height field wants a sample count divisible by its block size, and
    // powers of two are what it stores most efficiently.
    static constexpr int kMinResolution = 32;
    static constexpr int kMaxResolution = 1024;

    enum SculptTool : int {
        Sculpt_Raise   = 0,
        Sculpt_Lower   = 1,
        // Pulls every sample under the brush toward their average, which is what
        // removes the stair-stepping a raise brush leaves behind.
        Sculpt_Smooth  = 2,
        // Pulls toward a fixed height, for plateaus and building pads.
        Sculpt_Flatten = 3,
        Sculpt_Count   = 4
    };

    static const char* sculpt_tool_name(int tool);

    TerrainComponent(Actor* owner, const std::string& name);
    virtual ~TerrainComponent();

    virtual void begin_play() override;
    // Deliberately empty: the collision body is created from begin_play and the
    // geometry is uploaded from the render thread. Nothing here needs a tick.
    virtual void tick(float delta_time) override {}

    // --- Shape ---------------------------------------------------------------
    int get_resolution() const { return resolution; }
    float get_world_size() const { return world_size; }
    // Rebuilds at a new resolution or extent. Existing heights are resampled rather
    // than discarded, so raising the resolution of a sculpted terrain refines it
    // instead of flattening it.
    void resize(int new_resolution, float new_world_size);

    const std::vector<float>& get_heights() const { return heights; }
    // Height at a local-space xz position, bilinearly interpolated. Outside the
    // terrain it returns the nearest edge, which keeps queries from falling off.
    float sample_height(float local_x, float local_z) const;
    // Surface normal at a local-space xz position, from the height differences of
    // the neighbouring samples.
    Vector3 sample_normal(float local_x, float local_z) const;

    // Where a world-space ray meets the surface. Marches the ray in steps of one
    // cell and refines the crossing, which is what the editor's brush needs to know
    // where the mouse is pointing. Returns false if the ray misses.
    bool raycast(const DVector3& origin, const Vector3& direction, float max_distance,
                 DVector3& out_hit) const;

    // --- Sculpting -----------------------------------------------------------
    // All brush positions are in the terrain's local space, radius and strength in
    // metres. flatten_height is only read by the flatten tool.
    void sculpt(int tool, float local_x, float local_z, float radius, float strength,
                float flatten_height);
    // Raises the weight of one layer under the brush and renormalises the rest, so
    // the four weights always sum to full coverage.
    void paint_layer(int layer, float local_x, float local_z, float radius, float strength);
    // Adds or removes foliage coverage under the brush.
    void paint_foliage(float local_x, float local_z, float radius, float strength, bool erase);
    // Flattens everything back to zero and resets the splat to layer 0.
    void reset();

    // --- Material ------------------------------------------------------------
    std::string layer_texture_path[kLayerCount];
    // How many times each layer repeats across the whole terrain. Terrain textures
    // are tiled hard, or a 2048px texture stretched over 200 metres is a blur.
    float layer_tiling[kLayerCount] = { 40.0f, 40.0f, 40.0f, 40.0f };
    float metallic = 0.0f;
    float roughness = 0.85f;

    // Gameplay collision layer of the terrain's height field body.
    int collision_layer = 0;

    // --- Foliage -------------------------------------------------------------
    // Mesh scattered across the painted foliage coverage. Empty draws nothing.
    std::string foliage_mesh_path;
    // Instances per square metre at full painted coverage.
    float foliage_density = 0.6f;
    float foliage_min_scale = 0.8f;
    float foliage_max_scale = 1.4f;
    // Changing this reshuffles the scatter without repainting it.
    int foliage_seed = 1337;
    // Slopes steeper than this grow nothing, so trees do not stand out of cliffs.
    float foliage_max_slope_degrees = 35.0f;
    // Ceiling on how many instances are generated, so a fully painted large terrain
    // cannot ask for millions.
    int foliage_max_instances = 20000;

    // Per-instance transforms, regenerated whenever the coverage or the parameters
    // change. In the terrain's local space; the draw applies the component
    // transform on top.
    const std::vector<Matrix4x4>& get_foliage_instances() const;
    // Bumped every time the scatter is regenerated. The renderer keeps the instance
    // buffer it uploaded and re-uploads only when this changes, which is what stops
    // twenty thousand matrices crossing the bus twice a frame for a static forest.
    uint32_t get_foliage_version() const { return foliage_version; }
    std::shared_ptr<MeshResource> get_foliage_mesh() const;

    // --- GPU -----------------------------------------------------------------
    // Uploads whatever changed since the last call and draws the surface. Must be
    // called on the render thread.
    void render() const;
    // Binds the splat map and the four layer textures to the given texture units,
    // reporting which layers actually resolved. Called by the renderer just before
    // the draw.
    void bind_material(int splat_unit, int first_layer_unit, bool out_layer_present[kLayerCount]) const;
    unsigned int get_index_count() const { return index_count; }

    // Where the terrain sits. Always the owning actor's transform rather than this
    // component's own: a terrain added beside an existing mesh would otherwise stay
    // pinned at the origin while the actor it belongs to was moved away from it.
    const Transform& placement() const;

    // Local-space bounds, for culling and for the editor to frame the terrain.
    void get_local_bounds(Vector3& out_min, Vector3& out_max) const;

    // --- Persistence ---------------------------------------------------------
    // Heights, splat weights and foliage coverage are megabytes of numbers, so they
    // go into a sidecar binary file rather than into the scene's JSON. The scene
    // stores only this path and the handful of settings above.
    std::string data_path;
    bool save_data(const std::string& filepath) const;
    bool load_data(const std::string& filepath);

private:
    void rebuild_geometry();
    void recompute_normals_region(int x0, int z0, int x1, int z1);
    void mark_height_region_dirty(int x0, int z0, int x1, int z1);
    void mark_splat_dirty();
    void upload_pending() const;
    void rebuild_collision();
    void destroy_collision();
    void regenerate_foliage() const;

    int index_of(int x, int z) const { return z * resolution + x; }
    // Local-space xz of a sample, and the inverse.
    float sample_to_local(int index) const;
    void local_to_sample(float local_x, float local_z, float& out_fx, float& out_fz) const;

    int resolution = 128;
    float world_size = 200.0f;

    std::vector<float> heights;                 // resolution^2
    std::vector<unsigned char> splat;           // resolution^2 * 4, weights summing to 255
    std::vector<unsigned char> foliage_coverage; // resolution^2, 0..255

    // Mesh mirror of `heights`, kept so a sculpt can push only the rows it touched
    // rather than re-uploading the whole surface.
    mutable std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    unsigned int index_count = 0;

    mutable unsigned int vao = 0;
    mutable unsigned int vbo = 0;
    mutable unsigned int ebo = 0;
    mutable unsigned int splat_texture = 0;

    // Pending upload region, in sample coordinates. An empty region (x1 < x0) means
    // nothing to push.
    mutable int dirty_x0 = 0, dirty_z0 = 0, dirty_x1 = -1, dirty_z1 = -1;
    mutable bool geometry_dirty = true;
    mutable bool splat_dirty = true;

    mutable std::shared_ptr<TextureResource> layer_texture[kLayerCount];
    mutable bool layer_requested[kLayerCount] = { false, false, false, false };
    mutable std::string layer_requested_path[kLayerCount];

    mutable std::shared_ptr<MeshResource> foliage_mesh;
    mutable std::string foliage_requested_path;
    mutable std::vector<Matrix4x4> foliage_instances;
    mutable bool foliage_dirty = true;
    mutable uint32_t foliage_version = 1;

    // Jolt body id for the height field, or the invalid id.
    uint32_t body_id = 0xFFFFFFFF;
};
