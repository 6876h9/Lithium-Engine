#pragma once

#include <functional>

#include "core/math.hpp"
#include "world/static_mesh_component.hpp"
#include "world/light_components.hpp"
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include "renderer/tesla.hpp"
#include "renderer/render_profiler.hpp"
#include "renderer/material_shader.hpp"
// Global illumination pipeline selection.
//
// Off and SSGI are implemented. VXGI and HardwareRT are declared and plumbed, but
// this renderer is OpenGL-only and has no voxelization pass or ray-tracing backend,
// so both report unavailable and fall back to SSGI rather than silently pretending
// to be something they are not.
enum class GIMode {
    Off = 0,
    SSGI = 1,
    VXGI = 2,
    HardwareRT = 3
};

// One Static SLR volume, resolved into renderer-space data for the volumetric pass.
struct SLRVolumeInstance {
    // Camera-relative world -> volume local space. The pass raymarches in world space
    // and tests each sample against a unit box in local space, so this is the only
    // transform it needs.
    Matrix4x4 inv_model;
    // Direction the light travels, in camera-relative world space. Drives the phase
    // function, which is what makes a beam brighten when you look along it.
    Vector3 beam_dir = { 0.0f, -1.0f, 0.0f };
    Vector3 color = { 1.0f, 1.0f, 1.0f };
    float alpha = 1.0f;
    int shape = 0;   // 0 = box, 1 = cone
    float sharpness = 0.75f;
    float intensity = 1.0f;
    float falloff = 1.6f;
    float core = 0.65f;
};

class Renderer {
public:
    // Size of the vertex shader's bone palette. 128 mat4s is 2048 vertex uniform
    // components, which every driver exposing the GL 4.5 core profile this renderer
    // targets provides comfortably. The importer refuses to produce a skeleton
    // larger than this, so an out-of-range index can only come from a corrupt asset
    // - the shader clamps for that case rather than reading past the array.
    static constexpr int kMaxBones = 128;

    Renderer();
    ~Renderer();

    bool initialize(int width, int height);
    void begin_frame();
    void end_frame();
    void render_skybox();
    // Additively composites authored volumetric light beams onto the resolved scene.
    void render_slr_volumes(const std::vector<SLRVolumeInstance>& volumes);

    // HDRI Image-Based Lighting: loads an equirectangular .hdr environment map
    // and uses it (mipmapped, roughness-LOD sampled) for ambient diffuse/specular
    // instead of the analytic sky approximation. Pass an empty path / call
    // clear_environment_map() to fall back to the analytic sky again.
    bool load_environment_map(const std::string& hdr_path);
    void build_irradiance_map(const float* src, int src_width, int src_height);
    void build_prefiltered_specular(const float* src, int src_width, int src_height);
    void init_ssao_shaders();
    void init_bloom_shaders();
    void init_slr_shader();
    void clear_environment_map();

    // Eye adaptation: call once per frame (after the scene is resolved, before
    // resolve_fxaa()) with the frame's delta time to update the temporally
    // smoothed scene luminance used to drive automatic exposure.
    void update_exposure(float delta_time);

    // Offline Path Tracing
    void start_offline_render(int width, int height);
    void step_offline_render();
    void cancel_offline_render();
    void present_tesla();
    void ensure_tesla_present_target(int width, int height);

    // bone_matrices is the skinning palette for this draw, or null/empty for a
    // static mesh. It is passed per-draw rather than read off the component because
    // the render thread must not touch live animation state: the pose it draws is
    // the one snapshotted into the frame's RenderState.
    // lod_mesh substitutes a lower-detail mesh for this draw without changing what
    // the component owns; null draws the component's own mesh.
    void render_mesh(const StaticMeshComponent& mesh_component, const Transform& transform, const Vector3& color_override,
                      float metallic, float roughness, float clearcoat, float clearcoat_roughness, float sheen, float subsurface, float emissive,
                      bool is_invisible, bool is_selected = false,
                      const std::vector<Matrix4x4>* bone_matrices = nullptr,
                      const class MeshResource* lod_mesh = nullptr,
                      const class MaterialShader* custom_shader = nullptr,
                      const std::vector<MaterialShader::Value>* custom_shader_values = nullptr);
    // One particle, resolved for the GPU. Snapshotted on the logic thread rather
    // than read live off the component, because the render thread draws a frame the
    // logic thread has already moved on from.
    struct ParticleInstance {
        Vector3 position;   // world space
        float size = 1.0f;
        Vector4 color;      // rgb premultiplied by intensity, a is coverage
        float rotation = 0.0f;
    };

    // Particles are drawn forward, after lighting, into the resolve buffer - not
    // into the G-buffer. A deferred G-buffer has no way to represent a translucent
    // surface: writing a half-transparent spark into it would overwrite the albedo
    // and normal of whatever is behind it. Depth comes from sampling the scene depth
    // and fading, which also gives soft intersections for free.
    void render_particles(const std::vector<ParticleInstance>& instances, int blend_mode,
                          const std::string& texture_path);

    // --- Terrain -------------------------------------------------------------
    // Terrain writes the same G-buffer as everything else; only its material
    // differs, blending four tiling layers by a splat map instead of sampling one
    // diffuse texture. It uses the ordinary vertex layout, so the shadow pass draws
    // it through the same depth program as any other mesh.
    // Indirect light for the next draw, sampled from the probe grid at that object's
    // position. Set as renderer state rather than passed to every draw call, because
    // it applies uniformly to whichever of the four geometry programs runs next and
    // threading it through each signature would say the same thing four times.
    void set_ambient_cube(const Vector3 cube[6]);

    void render_terrain(const class TerrainComponent& terrain, const Transform& transform);
    void render_terrain_shadow(const class TerrainComponent& terrain, const Transform& transform);

    // Scattered foliage, drawn instanced: a field of ten thousand grass tufts is one
    // draw call with a per-instance transform stream, not ten thousand draws.
    // `instances` are in the terrain's local space and `base` places that space.
    void render_foliage(const class TerrainComponent& terrain, const class MeshResource& mesh,
                        const Transform& base);
    void render_foliage_shadow(const class TerrainComponent& terrain, const class MeshResource& mesh,
                               const Transform& base);

    void set_view_matrix(const Matrix4x4& view) { view_matrix = view; }
    void set_projection_matrix(const Matrix4x4& proj) { projection_matrix = proj; }
    void set_camera_pos(const DVector3& pos) { camera_pos = pos; } // LWC
    const DVector3& get_camera_pos() const { return camera_pos; }    // LWC origin

    // Invoked at intervals during initialize() so the caller can keep presenting a
    // loading frame. Renderer startup compiles every shader and convolves the
    // environment map, which is seconds of work with no frames in between otherwise.
    // The label names the phase just entered, or is null to mean "same phase as
    // before" - most call sites are progress ticks within a phase, not new phases.
    void set_loading_callback(std::function<void(const char*)> cb) { loading_progress_callback = std::move(cb); }
    void set_light_space_matrix(const Matrix4x4& lightSpace) { light_space_matrix = lightSpace; }
    const Matrix4x4& get_projection_matrix() const { return projection_matrix; }
    void set_camera_position(const Vector3& pos) { camera_position = pos; }

    Vector3 camera_position;

    // Per-pass CPU/GPU timings and draw counts for the frame. Public so the editor's
    // stats panel can read it without the renderer knowing about the UI.
    RenderProfiler profiler;

    // Framebuffer operations
    void create_fbo(int width, int height);
    void destroy_fbo();

    int pending_fbo_width = 0;
    int pending_fbo_height = 0;
    void request_fbo_resize(int w, int h) {
        pending_fbo_width = w;
        pending_fbo_height = h;
    }

    void bind_fbo();
    void unbind_fbo();
    void bind_resolve_fbo();
    void unbind_resolve_fbo();
    unsigned int get_viewport_texture() const {
        if (is_offline_rendering && tesla_present_texture != 0) return tesla_present_texture;
        return enable_msaa ? fxaa_texture : resolve_texture;
    }

private:
    unsigned int geometry_shader_program = 0;
    unsigned int lighting_shader_program = 0;
    
    Matrix4x4 view_matrix;
    Matrix4x4 projection_matrix;

    int mvp_location = -1;
    int model_location = -1;
    int camera_pos_location = -1;
    int color_override_location = -1;
    int ue4_lighting_location = -1;
    int num_lights_location = -1;
    int ray_tracing_location = -1;
    int metallic_location = -1;
    int roughness_location = -1;
    int has_diffuse_texture_location = -1;
    int diffuse_texture_location = -1;
    int clearcoat_location = -1;
    int clearcoat_roughness_location = -1;
    int sheen_location = -1;
    int subsurface_location = -1;
    int emissive_location = -1;
    int skinned_location = -1;
    int bones_location = -1;
    int lightmap_location = -1;
    int has_lightmap_location = -1;
    int ambient_cube_location = -1;
    int terrain_ambient_cube_location = -1;
    int foliage_ambient_cube_location = -1;
    Vector3 current_ambient_cube[6];

public:
    bool enable_ue4_lighting = true;
    bool wireframe_mode = false;
    std::vector<LightComponent*> lights;
    
    // TAA / Upscaling variables
    // Screen-space reflections, toggleable at runtime from the Options menu.
    GIMode gi_mode = GIMode::SSGI;
    // Neither advanced path has a backend in this build; the UI reads these so it can
    // say so plainly instead of offering a mode that quietly does nothing.
    static constexpr bool vxgi_supported = false;
    static constexpr bool hardware_rt_supported = false;

    bool enable_ssr = true;
    bool enable_msaa = true;
    bool enable_taa = true;
    float upscaling_scale = 1.0f;
    bool enable_ray_tracing = false;
    bool enable_embree = false;
    bool enable_tesla = false;
    bool enable_lithite = false; // Disabling Nanite fallback for LLVMpipe compat (compute shader fails on this driver)

    // Plane struct for frustum culling
    struct Plane {
        Vector3 normal;
        float distance;
    };
    std::array<Plane, 6> frustum_planes;

    // --- Visibility ----------------------------------------------------------
    // Two independent culls, both operating on the same camera-relative bounding
    // sphere / box the render command carries.
    //
    // Frustum culling is exact and free: an object outside the view volume cannot
    // contribute a pixel. Occlusion culling is a GPU query against the depth this
    // frame's geometry pass already produced, and its answer is used on the *next*
    // frame - reading a query the frame it was issued stalls the pipeline until the
    // GPU has caught up, which costs more than the draws it would save.
    bool enable_frustum_culling = true;
    bool enable_occlusion_culling = true;

    // Rebuilds frustum_planes from the current projection * view. The planes are in
    // camera-relative world space, which is the space render_mesh already works in,
    // so the camera sits at the origin.
    void update_frustum_planes();
    // center is camera-relative. A sphere is used rather than the box because the
    // test is six dot products either way and a sphere needs no rotation.
    bool is_inside_frustum(const Vector3& center_relative, float radius) const;

    // Occlusion query pass. Runs with the G-buffer still bound and its depth intact:
    // each object's bounding box is drawn with depth testing on but colour and depth
    // writes off, and the query reports whether any part of it would have been
    // visible.
    void begin_occlusion_pass();
    // key identifies the object across frames; the mesh component pointer is stable
    // and unique, which is all this needs.
    void submit_occlusion_test(const void* key, const Matrix4x4& relative_model,
                               const Vector3& local_min, const Vector3& local_max);
    void end_occlusion_pass();
    // Result of the most recently completed query for this object. False for
    // anything never tested, so a new object is drawn rather than hidden.
    bool was_occluded(const void* key) const;
    // Drops every recorded result, so nothing stays hidden across a scene change.
    void reset_occlusion_state();

    // Objects skipped this frame, for the profiler panel.
    int culled_by_frustum = 0;
    int culled_by_occlusion = 0;

    // LWC
    DVector3 camera_pos = {0.0, 0.0, 0.0};

    // FBO handles
    unsigned int gBuffer_fbo = 0;
    unsigned int gPosition = 0;
    unsigned int gNormal = 0;
    unsigned int gAlbedoSpec = 0;
    unsigned int gPBR = 0; // r: metallic, g: roughness, b: emission
    // Baked indirect light written by every geometry-pass shader: the lightmap for
    // static surfaces, the probe grid's ambient cube for everything else.
    unsigned int gBakedGI = 0;
    unsigned int gDepth = 0;
    
    // Legacy resolve FBO (now acts as the lighting pass output before post-processing)
    unsigned int resolve_fbo = 0;
    unsigned int resolve_texture = 0;
    unsigned int resolve_depth_texture = 0;
    unsigned int velocity_texture = 0;

    // TAA History Buffers
    unsigned int history_fbo[2] = {0, 0};
    unsigned int history_texture[2] = {0, 0};
    int frame_index = 0;
    Matrix4x4 prev_view_projection;
    Matrix4x4 unjittered_projection;

    unsigned int taa_shader_program = 0;

    // TESLA - unbiased path tracer. Owns its own scene, BVH, accumulation buffer
    // and present pass; see renderer/tesla.hpp.
    TeslaRenderer tesla;
    bool is_offline_rendering = false;

    int get_offline_sample_count() const { return tesla.samples_done(); }
    int get_offline_target_samples() const { return tesla.settings().target_samples; }
    bool is_offline_complete() const { return tesla.is_complete(); }
    const char* offline_backend_name() const { return tesla.backend_name(); }

    // Manual exposure for the TESLA present pass. The raster path's auto-exposure
    // is driven by a luminance texture the geometry pass fills in, and that pass
    // does not run in TESLA mode.
    float tesla_exposure = 1.0f;

    bool tesla_auto_exposure = true;
    unsigned int tesla_present_program = 0;
    unsigned int tesla_lum_program = 0;
    unsigned int tesla_lum_fbo = 0;
    unsigned int tesla_lum_texture = 0;
    unsigned int tesla_present_fbo = 0;
    unsigned int tesla_present_texture = 0;
    int tesla_present_width = 0;
    int tesla_present_height = 0;
    Matrix4x4 tesla_last_view_proj = Matrix4x4::identity();
    Vector3 tesla_last_camera_pos = { 1e30f, 1e30f, 1e30f };

    // Content signature of the committed TESLA scene, so a rebuild happens when the
    // scene actually changes rather than when the render mode is toggled.
    uint64_t tesla_scene_signature = 0;
    // TESLA traces in a scene-centred frame; this is where that frame sits in the
    // world, and the camera is offset by it before tracing.
    DVector3 tesla_world_origin = { 0.0, 0.0, 0.0 };

    unsigned int sky_shader_program = 0;
    int sky_forward_loc = -1;
    int sky_right_loc = -1;
    int sky_up_loc = -1;
    int sky_fov_tan_loc = -1;
    int sky_aspect_loc = -1;
    int sky_sun_dir_loc = -1;
    int sky_time_loc = -1;
    int sky_enable_3d_clouds_loc = -1;

    unsigned int culling_compute_program = 0;
    unsigned int cluster_ssbo = 0;
    unsigned int command_ssbo = 0;

    float sky_time_override = -1.0f;

    // FXAA handles
    unsigned int fxaa_fbo = 0;
    unsigned int fxaa_texture = 0;
    unsigned int fxaa_shader_program = 0;
    
    // God Rays handles
    unsigned int god_rays_fbo = 0;
    unsigned int god_rays_texture = 0;
    unsigned int god_rays_shader_program = 0;
    int god_rays_screen_texture_loc = -1;
    int god_rays_sun_pos_loc = -1;
    unsigned int composite_shader_program = 0;
    int composite_base_texture_loc = -1;
    int composite_blend_texture_loc = -1;

    unsigned int quad_vao = 0, quad_vbo = 0;
    void setup_quad();

    // Unit-cube geometry and the trivial program used to draw bounding boxes for
    // occlusion queries. Kept separate from the depth pass, which writes depth and
    // carries skinning uniforms this does not want.
    void init_particle_shader();
    unsigned int particle_shader_program = 0;
    unsigned int particle_vao = 0;
    unsigned int particle_quad_vbo = 0;
    unsigned int particle_instance_vbo = 0;
    size_t particle_instance_capacity = 0;
    int particle_view_projection_location = -1;
    int particle_view_location = -1;
    int particle_inv_projection_location = -1;
    int particle_scene_depth_location = -1;
    int particle_texture_location = -1;
    int particle_has_texture_location = -1;
    int particle_soft_fade_location = -1;
    int particle_viewport_location = -1;

    void init_terrain_shader();
    void init_foliage_shaders();
    // Uploads `instances` into the shared instance buffer and builds a vertex array
    // combining the mesh's own buffers with it. Returns false if either is missing.
    bool bind_foliage_geometry(const class TerrainComponent& terrain, const class MeshResource& mesh);

    unsigned int terrain_shader_program = 0;
    int terrain_mvp_location = -1;
    int terrain_model_location = -1;
    int terrain_light_space_location = -1;
    int terrain_splat_location = -1;
    int terrain_layer_location[4] = { -1, -1, -1, -1 };
    int terrain_tiling_location = -1;
    int terrain_layer_present_location = -1;
    int terrain_metallic_location = -1;
    int terrain_roughness_location = -1;
    int terrain_ue4_location = -1;

    unsigned int foliage_shader_program = 0;
    int foliage_view_projection_location = -1;
    int foliage_base_model_location = -1;
    int foliage_light_space_location = -1;
    int foliage_metallic_location = -1;
    int foliage_roughness_location = -1;
    int foliage_ue4_location = -1;
    int foliage_texture_location = -1;
    int foliage_has_texture_location = -1;
    unsigned int foliage_depth_program = 0;
    int foliage_depth_view_projection_location = -1;
    int foliage_depth_base_model_location = -1;
    unsigned int foliage_vao = 0;
    unsigned int foliage_instance_vbo = 0;
    size_t foliage_instance_capacity = 0;
    // Instances actually uploaded by the last bind, so the draw knows its count.
    int foliage_instance_count = 0;
    // Which terrain's scatter is currently in the instance buffer, and at what
    // version. Instances live in the terrain's own space and the base transform is a
    // uniform, so a static forest is uploaded once and then only drawn.
    const void* foliage_uploaded_owner = nullptr;
    uint32_t foliage_uploaded_version = 0;

    void init_occlusion_resources();
    unsigned int occlusion_program = 0;
    unsigned int occlusion_vao = 0;
    unsigned int occlusion_vbo = 0;
    unsigned int occlusion_ebo = 0;
    int occlusion_mvp_location = -1;
    bool occlusion_pass_active = false;
    // Incremented per pass, so records not submitted this frame can be told apart
    // from ones that were and reset to visible rather than staying stale.
    unsigned int occlusion_frame = 0;

    struct OcclusionRecord {
        unsigned int query = 0;
        bool visible = true;
        bool query_pending = false;
        unsigned int last_frame = 0;
    };
    std::unordered_map<const void*, OcclusionRecord> occlusion_records;
    void render_god_rays();
    void render_ssao();
    void render_bloom();
    void resolve_fxaa();
    
    // Shadow handles
    unsigned int shadow_fbo = 0;
    unsigned int shadow_depth_map = 0;
    unsigned int depth_shader_program = 0;
    int depth_model_location = -1;
    int depth_light_space_location = -1;
    int depth_skinned_location = -1;
    int depth_bones_location = -1;
    Matrix4x4 light_space_matrix;

    void init_shadow_map();
    void begin_shadow_pass();
    void end_shadow_pass();
    // Skinned casters need the same palette as the geometry pass, or a character's
    // shadow stays frozen in bind pose while the character itself animates.
    void render_mesh_shadow(const StaticMeshComponent& mesh_component, const Transform& transform,
                             const std::vector<Matrix4x4>* bone_matrices = nullptr,
                             const class MeshResource* lod_mesh = nullptr);

    // Uploads the palette and the uSkinned flag to the currently bound program.
    // Returns true if the draw should be skinned.
    bool apply_skinning_uniforms(int skinned_uniform, int bones_uniform, const std::vector<Matrix4x4>* bone_matrices);

    int fbo_width = 0;
    int fbo_height = 0;

    unsigned int compile_shaders(const std::string& vertex_src, const std::string& fragment_src);

    // HDRI environment map (equirectangular, mipmapped for roughness-based LOD sampling)
    unsigned int env_map_texture = 0;
    // CPU-side copy of the same pixels. The GPU path tracer samples the texture
    // above, but the CPU one needs the data in main memory, and stb frees its buffer
    // as soon as the upload is done.
    // Path the current environment came from, so a scene can record its own lighting.
    std::string env_map_path;
    std::vector<float> env_map_cpu;   // RGB float, bottom-up, same layout as the texture
    int env_map_width = 0;
    int env_map_height = 0;
    // Cosine-convolved diffuse irradiance, built on the CPU at load time. Small
    // equirect (irradiance is very low frequency, so it needs almost no
    // resolution). Replaces the old trick of sampling the environment map's
    // coarsest mip, which was a single averaged texel and therefore gave every
    // surface normal the exact same ambient colour.
    unsigned int env_irradiance_texture = 0;
    // GGX-prefiltered specular environment. Mip N holds the environment convolved
    // with the GGX lobe for roughness N/(mips-1), which is the "prefiltered
    // environment map" half of the split-sum IBL approximation. A plain mip chain
    // (a box filter) is not the right kernel and leaves the sun point-like at low
    // roughness, which is what produced sparkling fireflies in reflections.
    std::function<void(const char*)> loading_progress_callback;
    void report_loading_progress(const char* label = nullptr) { if (loading_progress_callback) loading_progress_callback(label); }

    unsigned int env_prefiltered_texture = 0;
    float env_prefiltered_max_lod = 0.0f;
    bool has_env_map = false;
    float env_map_max_lod = 0.0f;
    int env_map_location = -1;
    int has_env_map_location = -1;
    int env_map_max_lod_location = -1;
    int irradiance_map_location = -1;
    int prefiltered_env_location = -1;
    // Shadow-map metrics, needed by PCSS to convert a normalised depth difference
    // into a real penumbra width in shadow-map texels.
    float shadow_texel_world_size = 1.0f;
    float shadow_depth_range = 1.0f;

public:
    // Aerial-perspective fog. Density is per world unit; height falloff controls how
    // quickly the fog thins out above fog_height.
    float fog_density = 0.018f;
    float fog_height = 0.0f;
    float fog_height_falloff = 0.12f;
private:

    // Ambient occlusion / screen-space GI. Computed in its own half-resolution pass
    // and then blurred, rather than inline in the final post shader: the estimator is
    // stochastic, and evaluating it per-pixel with a per-pixel random rotation and no
    // spatial filter put its raw sampling noise straight on screen.
    // Bloom mip pyramid: threshold/prefilter into mip 0, progressively downsample
    // with the Karis-weighted 13-tap filter, then tent-filter upsample back while
    // accumulating. This is what gives a wide, soft, physically-plausible glow;
    // sampling a handful of taps in the final shader can only ever produce a small
    // hard halo (and, when the taps are axis-aligned, a visible cross).
    static const int kBloomMips = 6;
    unsigned int bloom_fbo[kBloomMips] = {};
    unsigned int bloom_texture[kBloomMips] = {};
    int bloom_w[kBloomMips] = {};
    int bloom_h[kBloomMips] = {};
    unsigned int slr_shader_program = 0;
    unsigned int bloom_prefilter_program = 0;
    unsigned int bloom_down_program = 0;
    unsigned int bloom_up_program = 0;

    unsigned int ssao_fbo = 0, ssao_texture = 0;
    unsigned int ssao_blur_fbo = 0, ssao_blur_texture = 0;
    // The lighting pass consumes the previous frame's AO, so there is one frame
    // after startup or a resize where ssao_blur_texture holds nothing. Ambient is
    // left unoccluded until render_ssao() has actually written it, rather than
    // multiplied by a cleared buffer.
    bool ssao_history_valid = false;

public:
    // How strongly ambient occlusion darkens indirect light. 0 disables it, 1 lets a
    // fully occluded point receive no ambient at all. Adjustable because AO strength
    // is a look decision, not a correctness one - and because it now only touches
    // indirect light, it can be pushed much harder than when it scaled the whole
    // image without turning direct sunlight muddy.
    float ssao_strength = 1.0f;

    // Multiplies the auto-exposure result. A deliberately dark scene can end up
    // genuinely unplayable on a dim panel, and auto-exposure cannot help - it is
    // already doing what it was asked. This is the viewer's own gamma knob, the one
    // every horror game ships, rather than an attempt to second-guess the art.
    float exposure_bias = 1.0f;

private:
    unsigned int ssao_shader_program = 0;
    unsigned int ssao_blur_shader_program = 0;
    int ssao_width = 0, ssao_height = 0;
    int sky_env_map_location = -1;
    int sky_has_env_map_location = -1;
    int sky_mode_loc = -1;
    int sky_void_color_loc = -1;

    // Eye adaptation: ping-ponged 1x1 luminance textures holding the temporally
    // smoothed log-average scene luminance, plus the tiny shader that updates them.
    unsigned int exposure_fbo[2] = {0, 0};
    unsigned int exposure_texture[2] = {0, 0};
    int exposure_current_index = 0;
    bool exposure_initialized = false;
    unsigned int luminance_adapt_shader_program = 0;
    int luminance_adapt_prev_loc = -1;
    int luminance_adapt_screen_loc = -1;
    int luminance_adapt_dt_loc = -1;
    void init_exposure_resources();
};
