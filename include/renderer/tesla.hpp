#pragma once

// TESLA - unbiased Monte Carlo path tracer.
//
// The estimator here is a straight application of the rendering equation with no
// fudge factors: every sampled direction is divided by the pdf it was drawn from,
// every lobe is energy-normalised, and paths terminate by Russian roulette rather
// than by a fixed depth cut. That is what makes the result *unbiased* - the mean of
// N samples converges to the true solution as N grows, and stays centred on it at
// every N.
//
// Two backends evaluate the identical integrator:
//   - CPU: multithreaded over tiles, traverses the BVH below (or Embree if the
//     caller supplies a scene).
//   - GPU: a fragment shader accumulating into an RGBA32F target with additive
//     blending. Needs only GL 3.3 + texture buffer objects, so it runs anywhere the
//     rest of the renderer runs.
//
// Both use the same PCG32 stream and the same sampling routines, so a CPU render and
// a GPU render of the same scene converge to the same image.

#include "core/math.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Scene description
// ---------------------------------------------------------------------------

struct TeslaMaterial {
    Vector3 base_color = { 0.8f, 0.8f, 0.8f };
    float   metallic   = 0.0f;
    float   roughness  = 0.5f;
    // Index into the renderer's texture array, or -1 for an untextured material.
    // Multiplies base_color, matching what the rasteriser's geometry pass does.
    int     albedo_texture = -1;
    // Emitted radiance. Non-zero makes the triangle a light source that is both
    // hit by scattered rays and explicitly sampled, combined with MIS.
    Vector3 emission   = { 0.0f, 0.0f, 0.0f };
};

struct TeslaTriangle {
    Vector3 v0, v1, v2;
    Vector3 n0, n1, n2;
    Vector2 uv0, uv1, uv2;
    int material = 0;
};

enum class TeslaLightType : int {
    Directional = 0,
    Point       = 1,
    Spot        = 2,
    Area        = 3,
};

// Analytic lights are not part of the BVH, so a scattered ray can never hit one.
// They are therefore sampled by next-event estimation only, with MIS weight 1 -
// which is exactly unbiased for this light model, and is why no double counting
// correction is needed for them.
struct TeslaLight {
    TeslaLightType type = TeslaLightType::Point;

    Vector3 color     = { 1.0f, 1.0f, 1.0f };
    float   intensity = 1.0f;

    Vector3 position  = { 0.0f, 0.0f, 0.0f };
    Vector3 direction = { 0.0f, -1.0f, 0.0f };   // direction the light travels

    // Point / spot: physical emitter radius. Non-zero gives a real penumbra
    // because the sampled point is drawn over the sphere, not from a single point.
    float radius = 0.0f;

    // Directional: angular radius of the source disc in radians. The real sun is
    // ~0.00465 rad. Non-zero gives soft shadow edges that scale with distance.
    float angular_radius = 0.0f;

    // Spot cone, cosines of the half-angles.
    float cos_inner = 0.9762f;
    float cos_outer = 0.9537f;

    // Area: half-extent vectors spanning the rectangle, centred on `position`.
    Vector3 u_axis = { 1.0f, 0.0f, 0.0f };
    Vector3 v_axis = { 0.0f, 0.0f, 1.0f };
};

// Smooth gradient environment. The sun disc deliberately lives in the directional
// light rather than in here: putting it in both is what makes naive tracers count
// the sun twice.
struct TeslaSky {
    Vector3 zenith    = { 0.10f, 0.30f, 0.80f };
    Vector3 horizon   = { 0.50f, 0.70f, 1.00f };
    Vector3 ground    = { 0.15f, 0.12f, 0.10f };
    float   intensity = 1.0f;
    bool    enabled   = true;

    // Equirectangular environment map. When present it replaces the gradient above
    // entirely: in a path tracer the background and the ambient light are the same
    // quantity, so a scene lit by an HDRI has to be able to see it.
    int          env_width  = 0;
    int          env_height = 0;
    unsigned int env_texture = 0;        // the same image as a GL texture, for the GPU backend
};

// ---------------------------------------------------------------------------
// BVH
// ---------------------------------------------------------------------------

// 32 bytes, laid out as two vec4s so the same array uploads straight to the GPU.
struct TeslaBVHNode {
    Vector3 bmin;
    int32_t left_first = 0;   // interior: index of left child; leaf: first primitive
    Vector3 bmax;
    int32_t count = 0;        // 0 => interior node
};

class TeslaBVH {
public:
    void build(const std::vector<TeslaTriangle>& tris);
    void clear();

    bool empty() const { return nodes_.empty(); }
    const std::vector<TeslaBVHNode>& nodes() const { return nodes_; }
    const std::vector<int32_t>&      indices() const { return indices_; }

private:
    struct BuildPrim {
        Vector3 centroid;
        Vector3 bmin, bmax;
    };

    void subdivide(int node_index, const std::vector<BuildPrim>& prims, int depth);

    std::vector<TeslaBVHNode> nodes_;
    std::vector<int32_t>      indices_;
};

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct TeslaSettings {
    int target_samples = 512;

    // 0 = unlimited. Russian roulette is what terminates paths, and it is unbiased;
    // a non-zero cap re-introduces truncation bias, so it exists only as a safety
    // valve for pathological scenes.
    int max_depth = 0;

    // Roulette starts after this many bounces so near-camera paths stay low-noise.
    int   rr_start_depth  = 3;
    float rr_max_survival = 0.95f;

    bool  use_gpu = true;
    float cpu_time_budget_ms = 24.0f;   // per step(), keeps the editor responsive

    // Clamps the *indirect* contribution of a single path. Off by default because
    // any non-zero value is a bias source; exposed because fireflies are sometimes
    // worth the trade.
    float firefly_clamp = 0.0f;

    // --- Denoising -----------------------------------------------------------
    // Runs Intel Open Image Denoise over the accumulated estimate. The estimator
    // itself stays unbiased; this is a post-process on the image it produced, and
    // the raw accumulation is kept intact so more samples can still be added.
    //
    // Off while accumulating and on at completion is the useful default: denoising
    // every step costs more than the samples it saves, but a finished render
    // essentially always wants it.
    bool denoise = true;
    // Denoise each time the preview updates, not only when the render completes.
    // Expensive, and worth it when someone is dialling in lighting interactively.
    bool denoise_while_rendering = false;
};

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

class TeslaRenderer {
public:
    TeslaRenderer();
    ~TeslaRenderer();

    TeslaRenderer(const TeslaRenderer&) = delete;
    TeslaRenderer& operator=(const TeslaRenderer&) = delete;

    // --- scene construction ---
    void begin_scene();
    int  add_material(const TeslaMaterial& m);
    void add_triangle(const TeslaTriangle& t);
    void add_light(const TeslaLight& l);
    void set_sky(const TeslaSky& s);
    // Registers a GL texture with the tracer, reading it back and resampling it into
    // the shared texture array. Returns a material-usable index, or -1. Repeated
    // calls with the same GL id return the same index rather than re-reading it.
    int  add_texture(unsigned int gl_texture);
    // Equirectangular environment. Pass a null pointer / zero texture to fall back
    // to the gradient sky.
    void set_environment(const float* pixels, int width, int height, unsigned int gl_texture);
    // Builds the BVH and the emitter sampling table, and uploads to the GPU if the
    // GPU backend is available. Resets accumulation.
    void end_scene();

    // Content hash of the geometry/lights/materials last committed. The caller can
    // compare it against a freshly gathered scene to know whether a rebuild is
    // actually needed, instead of rebuilding on a mode toggle.
    uint64_t scene_hash() const { return scene_hash_; }

    // --- frame setup ---
    void set_camera(const Matrix4x4& view, const Matrix4x4& projection, const Vector3& position);
    void resize(int width, int height);
    void reset_accumulation();

    // --- driving the render ---
    void step();
    bool is_complete() const { return samples_done_ >= settings_.target_samples; }
    int  samples_done() const { return samples_done_; }
    int  width() const  { return width_; }
    int  height() const { return height_; }

    // --- Denoising -----------------------------------------------------------
    // True when the build has Open Image Denoise and its device came up. False on a
    // build without the SDK, so callers can grey the option out rather than offer
    // something that cannot run.
    bool denoise_available() const;
    // Runs the filter over the current estimate. Returns false when unavailable, or
    // when nothing has been accumulated yet. The accumulation is not modified.
    bool run_denoiser();
    // The texture to present: the denoised result when one is current, otherwise the
    // raw accumulation. Callers should prefer this over accumulation_texture().
    unsigned int display_texture() const;
    // Whether the denoised texture reflects the current sample count.
    bool denoised_is_current() const { return denoised_valid_; }

    // RGBA32F: rgb = summed radiance, a = sample count. Divide to get the estimate.
    unsigned int accumulation_texture() const { return accum_texture_; }

    // Current estimate for one pixel from the CPU backend, i.e. the running mean of
    // the samples taken so far. Returns black for the GPU backend, whose accumulator
    // lives in the texture above.
    Vector3 pixel_estimate(int x, int y) const {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return { 0.0f, 0.0f, 0.0f };
        size_t i = static_cast<size_t>(y) * width_ + x;
        if (i >= accum_.size() || accum_counts_[i] <= 0.0f) return { 0.0f, 0.0f, 0.0f };
        return accum_[i] / accum_counts_[i];
    }

    TeslaSettings& settings() { return settings_; }
    const TeslaSettings& settings() const { return settings_; }

    bool gpu_available() const { return gpu_ready_; }
    bool gpu_active() const { return gpu_ready_ && settings_.use_gpu; }
    const char* backend_name() const { return gpu_active() ? "GPU" : "CPU"; }

    // Initialises the GL-side resources. Safe to call more than once. Must run on
    // the thread owning the GL context.
    void initialize_gpu();
    void shutdown_gpu();

    int triangle_count() const { return static_cast<int>(triangles_.size()); }

private:
    // --- CPU integrator ---
    void step_cpu();
    void render_tile(int tile_index, int sample_index);
    // out_albedo / out_normal receive the first hit's surface albedo and shading
    // normal, for the denoiser. Null when the caller does not want them, which is
    // every path except primary camera rays.
    Vector3 trace(Vector3 origin, Vector3 dir, uint64_t& rng,
                  Vector3* out_albedo = nullptr, Vector3* out_normal = nullptr) const;

    // --- GPU integrator ---
    void step_gpu();
    void upload_scene_to_gpu();
    void upload_textures_to_gpu();
    void ensure_accum_target();

    // --- textures ---
    // Every texture is resampled to one square size so they can live in a single
    // GL_TEXTURE_2D_ARRAY. A per-pixel index into a sampler *array* is illegal in
    // GLSL 3.30 (the index must be dynamically uniform), but a layer index is just a
    // texture coordinate, so an array texture is the one thing that works here.
    std::vector<std::vector<unsigned char>> textures_;   // RGBA8, kTextureSize^2 each
    std::vector<unsigned int> texture_source_ids_;       // GL id each layer came from

    // --- scene data ---
    std::vector<TeslaTriangle> triangles_;
    std::vector<TeslaMaterial> materials_;
    std::vector<TeslaLight>    lights_;
    TeslaSky                   sky_;
    // Owned copy of the environment pixels. Aliasing the renderer's buffer would
    // dangle the moment a different HDRI is loaded and its vector reallocates.
    std::vector<float>         env_pixels_;
    TeslaBVH                   bvh_;

    // Emissive triangles, with a discrete CDF over surface area so an emitter is
    // chosen in proportion to how much light it can actually deliver.
    std::vector<int32_t> emitters_;
    std::vector<float>   emitter_cdf_;
    float                emitter_area_total_ = 0.0f;

    uint64_t scene_hash_ = 0;

    // --- camera ---
    Matrix4x4 inv_view_ = Matrix4x4::identity();
    Matrix4x4 inv_proj_ = Matrix4x4::identity();
    Vector3   camera_position_ = { 0.0f, 0.0f, 0.0f };

    // --- framebuffer ---
    int width_  = 0;
    int height_ = 0;
    std::vector<Vector3> accum_;        // CPU: summed radiance
    std::vector<float>   accum_counts_; // CPU: per-pixel sample count
    std::vector<float>   upload_scratch_;

    int samples_done_ = 0;
    TeslaSettings settings_;

    // --- CPU threading ---
    int tiles_x_ = 0;
    int tiles_y_ = 0;
    std::atomic<int> next_tile_{ 0 };

    // --- GPU resources ---
    // The accumulation target exists in both modes - the CPU backend uploads its
    // running sum into it - so GL setup and GPU-tracing readiness are separate.
    bool gl_initialized_ = false;
    bool gpu_ready_      = false;
    bool gpu_uploaded_   = false;

    unsigned int accum_fbo_     = 0;
    unsigned int accum_texture_ = 0;

    // --- denoiser ---
    // First-hit albedo and shading normal, averaged the same way radiance is. OIDN
    // uses them to tell a noisy shadow from a genuine texture edge, and feeding
    // them is the difference between preserved detail and a smeared image.
    std::vector<Vector3> aov_albedo_;
    std::vector<Vector3> aov_normal_;
    // Interleaved float3 planes handed to the filter, plus its output.
    std::vector<float> denoise_color_;
    std::vector<float> denoise_albedo_;
    std::vector<float> denoise_normal_;
    std::vector<float> denoise_output_;
    unsigned int denoised_texture_ = 0;
    bool denoised_valid_ = false;
    int  denoised_at_samples_ = -1;
    void ensure_denoise_target();
    void release_denoiser();

    unsigned int trace_program_ = 0;

    unsigned int tri_buffer_ = 0,  tri_tex_ = 0;
    unsigned int bvh_buffer_ = 0,  bvh_tex_ = 0;
    unsigned int idx_buffer_ = 0,  idx_tex_ = 0;
    unsigned int mat_buffer_ = 0,  mat_tex_ = 0;
    unsigned int lgt_buffer_ = 0,  lgt_tex_ = 0;
    unsigned int emt_buffer_ = 0,  emt_tex_ = 0;
    unsigned int albedo_lut_tex_ = 0;
    unsigned int texture_array_ = 0;
    bool textures_dirty_ = false;

    unsigned int quad_vao_ = 0, quad_vbo_ = 0;
};
