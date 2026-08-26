#pragma once

#include "core/math.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Actor;

// Baked global illumination: a lightmap for static surfaces and a grid of probes
// for everything that moves.
//
// The two exist because they answer the same question for different things. A wall
// never moves, so its indirect light can be solved once per texel and stored in a
// texture. A character walks through the room, so its indirect light has to be
// looked up from wherever it happens to be standing - which is what a probe grid is.
//
// Indirect light is solved with one bounce: rays leave each texel over the
// hemisphere, and a ray that escapes the scene collects sky light while a ray that
// hits something collects that surface's albedo lit by the sky. That is enough to
// produce the effect lightmaps exist for - a red wall tinting the floor beside it -
// without a full path tracer's convergence time.
class Lightmapper {
public:
    // Six directional colours, one per axis. Valve's ambient cube: cheaper than
    // spherical harmonics to evaluate, and - unlike an L1 SH probe - it cannot go
    // negative and paint black patches on a surface facing the wrong way.
    struct AmbientCube {
        Vector3 axis[6] = {}; // +x, -x, +y, -y, +z, -z

        // Irradiance arriving on a surface with this normal.
        Vector3 evaluate(const Vector3& normal) const {
            const Vector3 squared = { normal.x * normal.x, normal.y * normal.y, normal.z * normal.z };
            const Vector3& x = (normal.x >= 0.0f) ? axis[0] : axis[1];
            const Vector3& y = (normal.y >= 0.0f) ? axis[2] : axis[3];
            const Vector3& z = (normal.z >= 0.0f) ? axis[4] : axis[5];
            return { x.x * squared.x + y.x * squared.y + z.x * squared.z,
                     x.y * squared.x + y.y * squared.y + z.y * squared.z,
                     x.z * squared.x + y.z * squared.y + z.z * squared.z };
        }
    };

    struct BakeSettings {
        // Atlas edge in pixels. Everything static in the scene shares this one
        // texture, so a large level needs a large atlas or coarse texels.
        int atlas_size = 1024;
        // Lightmap resolution, in texels per world unit. This and the atlas size
        // together decide how much of the level fits.
        float texels_per_unit = 6.0f;
        // Hemisphere rays per texel. The noise floor falls as the square root of
        // this, so doubling quality costs four times the rays.
        int rays_per_texel = 64;
        // Probe grid spacing, in world units.
        float probe_spacing = 4.0f;
        bool bake_probes = true;
        // Ambient sky colour used when a ray escapes the scene. Multiplied into
        // everything, so this is the overall brightness control.
        Vector3 sky_color = { 0.45f, 0.55f, 0.75f };
        float sky_intensity = 1.0f;
        // Direct sunlight. Direction is where the light travels.
        Vector3 sun_direction = { -0.4f, -0.8f, -0.45f };
        Vector3 sun_color = { 1.0f, 0.96f, 0.88f };
        float sun_intensity = 2.2f;
    };

    static Lightmapper& get();

    // Bakes the scene. Only actors marked static contribute geometry and only they
    // receive a lightmap; everything else is covered by the probe grid. Progress is
    // reported through the callback so the caller can keep the window alive - a bake
    // is seconds to minutes, and a frozen window looks like a crash.
    bool bake(const std::vector<std::shared_ptr<Actor>>& actors, const BakeSettings& settings,
              const std::function<void(const char*, float)>& progress, std::string& out_report);

    void clear();
    bool is_baked() const { return atlas_width > 0 && !atlas_pixels.empty(); }

    // GPU handle for the baked atlas, uploaded on first use. Zero when nothing is
    // baked. Must be called on the render thread.
    unsigned int get_atlas_texture();
    int get_atlas_size() const { return atlas_width; }

    // Lightmap UVs for one actor, parallel to its mesh vertices. Empty when the
    // actor was not part of the bake.
    const std::vector<Vector2>* get_actor_uvs(const std::string& actor_name) const;

    // Indirect light at a world position, blended from the surrounding probes.
    // Returns the sky colour when nothing is baked, so an unbaked scene is lit
    // flatly rather than black.
    AmbientCube sample_probes(const DVector3& position) const;

    // Hands each baked actor its lightmap uvs and clears them from everything else.
    // Called after a bake and after loading, because the uvs live here but the
    // vertex stream that uses them belongs to the mesh component.
    void apply_to_actors(const std::vector<std::shared_ptr<Actor>>& actors) const;

    bool save(const std::string& filepath) const;
    bool load(const std::string& filepath);

    const std::string& get_last_report() const { return last_report; }
    // Settings the last bake ran with, so the editor can show them back.
    const BakeSettings& get_settings() const { return settings; }

private:
    Lightmapper() = default;
    ~Lightmapper() = default;
    Lightmapper(const Lightmapper&) = delete;
    Lightmapper& operator=(const Lightmapper&) = delete;

    // --- Scene representation used during a bake ---------------------------
    struct Triangle {
        Vector3 position[3];
        Vector3 normal;
        Vector3 albedo;
    };

    // A bounding-volume hierarchy over the static triangles. Built with a median
    // split, which is not the best possible tree but is O(n log n) to build and
    // needs no surface-area heuristic - and a bake spends its time in ray traversal,
    // not in tree quality.
    struct BVHNode {
        Vector3 bounds_min, bounds_max;
        int left = -1;      // child index, or -1 for a leaf
        int first = 0;      // leaf: first triangle
        int count = 0;      // leaf: triangle count
    };

    void build_bvh();
    // Closest hit. Returns the triangle index or -1, filling the distance and
    // barycentric weights of the hit.
    int trace(const Vector3& origin, const Vector3& direction, float max_distance,
              float& out_distance) const;
    bool occluded(const Vector3& origin, const Vector3& direction, float max_distance) const;
    // Light arriving at a point with a given normal: direct sun plus one bounce of
    // sky light. The heart of the bake.
    Vector3 gather(const Vector3& position, const Vector3& normal, int ray_count,
                   unsigned int& random_state) const;

    std::vector<Triangle> triangles;
    std::vector<BVHNode> bvh;
    std::vector<int> triangle_order;

    // --- Results -----------------------------------------------------------
    struct ActorLightmap {
        std::string actor_name;
        std::vector<Vector2> uvs;
    };
    std::vector<ActorLightmap> actor_lightmaps;

    int atlas_width = 0;
    int atlas_height = 0;
    // Linear RGB, three floats per texel. Not stored as bytes: bounce light spans
    // orders of magnitude and quantising it to 8 bits bands every gradient.
    std::vector<float> atlas_pixels;
    unsigned int atlas_texture = 0;
    bool atlas_dirty = false;

    // Probe grid.
    DVector3 probe_origin = { 0.0, 0.0, 0.0 };
    float probe_spacing = 4.0f;
    int probe_count_x = 0, probe_count_y = 0, probe_count_z = 0;
    std::vector<AmbientCube> probes;

    BakeSettings settings;
    std::string last_report;
};
