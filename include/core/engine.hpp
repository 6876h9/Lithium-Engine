#pragma once

#include "audio/audio_engine.hpp"
#include "core/window.hpp"
#include "renderer/renderer.hpp"
#include "world/actor.hpp"
#include "core/browser.hpp"
#include "core/editor.hpp"
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include "core/threading/task_graph.hpp"
#include "world/static_mesh_component.hpp"
#include "world/particle_emitter_component.hpp"
#include "renderer/material_shader.hpp"

struct RenderMeshCommand {
    std::shared_ptr<Actor> actor;
    StaticMeshComponent* mesh;
    Transform transform;
    // Skinning palette for this frame, empty for a static mesh. Copied rather than
    // referenced: the logic thread keeps advancing the pose while the render thread
    // is still drawing the previous one, and RenderState exists precisely so the
    // render thread never reads live scene state.
    std::vector<Matrix4x4> bone_matrices;
    // Mesh to draw instead of the component's own, chosen by the actor's LOD group
    // for this frame. Null means full detail. Held as a shared_ptr, not a raw one,
    // because the render thread draws this a frame after the logic thread picked it
    // and the resource must not be freed in between.
    std::shared_ptr<class MeshResource> lod_mesh;
    // Custom surface shader from the actor's material, already compiled. Null uses
    // the engine's standard shading. Resolved on the logic thread because loading
    // and compiling a shader is not something the render thread should discover
    // it needs to do mid-frame.
    std::shared_ptr<class MaterialShader> custom_shader;
    std::vector<MaterialShader::Value> custom_shader_values;
    // World-space bounding sphere and local-space box of what this draw covers.
    // Computed on the logic thread because reading the mesh resource's bounds from
    // the render thread would race the logic thread swapping that resource.
    // has_bounds is false while a mesh is still streaming, and nothing with unknown
    // bounds is ever culled - popping in late is worse than one extra draw.
    bool has_bounds = false;
    Vector3 bounds_center_world = { 0.0f, 0.0f, 0.0f };
    float bounds_radius_world = -1.0f;
    Vector3 bounds_local_min = { 0.0f, 0.0f, 0.0f };
    Vector3 bounds_local_max = { 0.0f, 0.0f, 0.0f };
    // Indirect light at this object, sampled from the probe grid. Resolved here so
    // the render thread never has to look anything up mid-frame.
    Vector3 ambient_cube[6];
    Vector3 color;
    float metallic;
    float roughness;
    float clearcoat;
    float clearcoat_roughness;
    float sheen;
    float subsurface;
    float emissive;
    bool is_invisible;
    bool is_selected;
};

// A terrain snapshotted for the render thread. Only a pointer, like the mesh
// command above: the heightmap and its GPU buffers are owned by the component and
// are only ever touched from the thread that draws them.
struct RenderTerrainCommand {
    std::shared_ptr<Actor> actor;
    class TerrainComponent* terrain = nullptr;
    Transform transform;
    Vector3 ambient_cube[6];
};

struct RenderParticleCommand {
    std::shared_ptr<Actor> actor;
    // Resolved on the logic thread. The component keeps simulating while the render
    // thread draws, so reading its live particle array from there would be a race -
    // and sorting it for alpha blending would be a race that also writes.
    std::vector<Renderer::ParticleInstance> instances;
    int blend_mode = 0;
    std::string texture_path;
};

struct RenderLightCommand {
    std::shared_ptr<Actor> actor;
    LightComponent* light;
};

// A Static SLR volume snapshotted for the render thread.
struct RenderSLRCommand {
    std::shared_ptr<Actor> actor;
    Transform transform;
    Vector3 color;
    float alpha;
    int shape;
    float sharpness;
    float intensity;
    float falloff;
    float core;
};

struct RenderState {
    DVector3 camera_pos;
    Matrix4x4 view_matrix;
    std::vector<RenderMeshCommand> meshes;
    std::vector<RenderParticleCommand> particles;
    std::vector<RenderTerrainCommand> terrains;
    std::vector<RenderLightCommand> lights;
    std::vector<RenderSLRCommand> slr_volumes;
    
    // Global render settings from DirectionalLightActor
    bool enable_taa = true;
    float upscaling_scale = 1.0f;
    bool enable_ray_tracing = false;
    bool enable_embree = false;
    bool enable_3d_clouds = false;
    int sky_mode = 0;
    Vector3 void_color = { 0.015f, 0.02f, 0.045f };
};

enum class EngineState {
    MainMenu,
    ProjectBrowser,
    Editor,
    PlayInEditor,
    PlayInEditorPaused
};

class Engine;
extern Engine* g_engine;

class Engine {
public:
    Engine();
    ~Engine();

    bool initialize(const std::string& initial_scene_path = "", bool standalone = false);
    // Brings up the heavy subsystems (renderer, resources, audio, physics, network).
    // Deliberately separate from initialize(): the menus are plain ImGui and need
    // nothing but a window, so they are shown first and this only runs once the user
    // has actually committed to opening a project. Safe to call more than once.
    bool initialize_runtime();
    // Draws the running script's HUD over the finished frame.
    void draw_script_hud();
    // Lays out and draws every UICanvasComponent in the scene over the finished
    // frame, then turns the widget events it produced into script calls.
    void draw_ui_canvases();
    void run();
    void shutdown();

    template <typename T, typename... Args>
    T* spawn_actor(const std::string& name, Args&&... args) {
        auto actor = std::make_shared<T>(name, std::forward<Args>(args)...);
        T* ptr = actor.get();
        actors.push_back(actor);
        if (is_running && current_state == EngineState::PlayInEditor) {
            ptr->begin_play();
        }
        return ptr;
    }

    Actor* spawn_actor_by_id(int id);
    void queue_destroy_actor(Actor* actor);

    // Scene changes requested from a script. Scripts run inside the parallel actor
    // tick, so they must not touch the actor list directly - spawning would
    // reallocate the vector the task graph is iterating, and queue_destroy_actor is
    // a bare push_back that two script threads could race on. These take the lock,
    // and apply_script_commands() drains them on the logic thread between frames.
    void script_request_spawn(int kind, double x, double y, double z);
    void script_request_destroy(Actor* actor);
    void apply_script_commands();

    void destroy_queued_actors();

    Renderer& get_renderer() { return *renderer; }
    Window& get_window() { return *window; }
    void load_project_template();
    void take_screenshot(const std::string& filepath);
    bool screenshot_requested = false;
    std::string screenshot_requested_path = "screenshot.bmp";

    // Set from --auto-screenshot=<path> for headless/CI capture: once
    // auto_screenshot_delay seconds of wall-clock render time have elapsed,
    // one screenshot is taken automatically and the flag is cleared.
    std::string auto_screenshot_path;
    float auto_screenshot_delay = -1.0f;
    // Skips the project-browser screen and launches straight into the selected
    // template. The browser otherwise needs a real mouse click, which makes
    // headless/CI runs non-deterministic (they only ever advanced when a stray
    // click happened to land on the launch button).
    bool auto_launch = false;
    float auto_screenshot_elapsed = 0.0f;
    
    std::vector<std::shared_ptr<Actor>>& get_actors() { return actors; }

private:
    std::unique_ptr<Window> window;
    std::unique_ptr<Renderer> renderer;
    std::vector<std::shared_ptr<Actor>> actors;
    std::vector<Actor*> actors_to_destroy;

    struct ScriptSpawnRequest {
        int kind = 0;
        double x = 0.0, y = 0.0, z = 0.0;
    };
    std::mutex script_command_mutex;
    std::vector<ScriptSpawnRequest> script_spawn_requests;
    std::vector<Actor*> script_destroy_requests;

    bool is_running = false;

    EngineState current_state = EngineState::MainMenu;
    // Wall-clock time at which all subsystems finished coming up.
    float startup_finished_time = 0.0f;

    // Loading indicator. There is no splash state: initialisation is the only time
    // this draws, and the engine goes straight to the main menu once it completes.
    // The pump still exists because renderer startup (shader compilation plus
    // environment convolution) is seconds of work during which nothing else pumps
    // SDL - without it the window is black and the WM marks it unresponsive.
    std::string loading_status = "Starting up";
    // False until initialize_runtime() has run. Everything that touches the renderer
    // or a subsystem has to check this, because the main menu and project browser now
    // draw before any of them exist.
    bool runtime_initialized = false;
    bool loading_quit_requested = false;
    Uint32 loading_last_present_ms = 0;
    void pump_loading(const char* status = nullptr);
    void draw_loading_ui();
    void present_loading_frame();
    void request_shutdown();
    void focus_main_window();

    // Play-In-Editor snapshot: the scene's mutable state, captured on Play and put
    // back on Stop without touching disk.
    struct ActorPlayState {
        Actor* actor = nullptr;
        Transform transform;
        Vector3 color;
        float metallic = 0.0f, roughness = 0.0f;
        float clearcoat = 0.0f, clearcoat_roughness = 0.0f;
        float sheen = 0.0f, subsurface = 0.0f, emissive = 0.0f;
        bool is_invisible = false;
    };
    std::vector<std::shared_ptr<Actor>> pie_actor_order;
    std::vector<ActorPlayState> pie_states;
    void capture_pie_snapshot();
    void restore_pie_snapshot();
public:
    const DVector3& get_camera_position() const { return camera_pos; }
    // Files dropped onto the window from the OS file manager, drained by the editor.
    std::vector<std::string> pending_dropped_files;
private:
    void draw_unsaved_changes_prompt();
    void draw_main_menu();
    void load_engine_options();
    void save_engine_options();
    void apply_engine_options();
public:
    void set_sky_hdri(const std::string& path);
    void bake_static_lighting();
    void load_static_bake();
private:
    int main_menu_tab = 0;
    bool pending_quit_prompt = false;
    unsigned int editor_logo_texture = 0;
    ProjectBrowser browser_ui;
    Editor editor_ui;

public:
    EngineConfig active_config;
    // Viewport camera properties
    // Script-driven HUD. A C-Minus script has no way to draw anything, so a game
    // written in one has no way to tell the player what it wants from them. The
    // script writes here and the engine renders it over the finished frame.
    struct ScriptHud {
        float pip[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };  // filled markers, e.g. objectives
        int   pip_count = 0;
        float vignette = 0.0f;                       // 0..1 screen-edge tint
        Vector3 vignette_color = { 1.0f, 0.0f, 0.0f };

        // A line of text, addressed by index into the scene's string table rather
        // than by content. C-Minus has no string type, and putting the sentences in
        // the engine would mean shipping one game's script in every build - so the
        // script names a line and the project supplies the words. -1 shows nothing.
        int   message_index = -1;
        float message_seconds_left = 0.0f;
    };
    ScriptHud script_hud;

    // Lines a script may put on screen, loaded from "<scene>.strings" beside the
    // scene file: one line of UTF-8 per index, blank lines kept so indices stay
    // stable when a line is cleared. Empty when a project ships no table, which
    // simply means hud_message() has nothing to show.
    std::vector<std::string> script_strings;
    void load_script_strings(const std::string& scene_path);

    // Sound files a script may trigger, from "<scene>.sounds" beside the scene: one
    // path per index, resolved relative to the working directory. Indexed for the
    // same reason the strings are - C-Minus cannot name a file.
    std::vector<std::string> script_sounds;
    void load_script_sounds(const std::string& scene_path);
    // Fires one-shot `index` at `volume`. Silently does nothing for an unknown index
    // or before audio is up, because a missing sound must not stop a game.
    void play_script_sound(int index, float volume);
    // One-shot voices a script started, owned so each can carry its own volume.
    // Reaped when they finish; see play_script_sound for why they are not left to
    // miniaudio's fire-and-forget path.
    std::vector<std::unique_ptr<ma_sound>> script_voices;

    // Whether a game UI widget swallowed the pointer during the last frame's canvas
    // pass. Read by viewport picking so a click on a button does not also select the
    // actor behind it.
    bool ui_consumed_mouse = false;

    DVector3 camera_pos = {0.0, 0.0, 5.0}; // Positive Z (facing towards center)
    Vector3 camera_rot = {0.0f, 0.0f, 0.0f}; // pitch (X), yaw (Y)
    float ortho_zoom = 10.0f;
    bool is_rmb_down = false;
private:
    
    // World Partition
    double chunk_size = 100.0;
    bool enable_world_partition = true;
    bool is_in_active_chunk(const DVector3& pos) const;
    
    std::string pie_backup_scene = "";

    Matrix4x4 get_view_matrix() const;

    void process_input();
    void update(float delta_time);
    void populate_render_state(RenderState& state);
    void render(RenderState& state, float render_delta_time);

    std::unique_ptr<Threading::TaskGraph> task_graph;

    // Double buffering and threading
    RenderState render_buffers[2];
    int logic_buffer_index = 0;
    int render_buffer_index = 1;
    std::mutex swap_mutex;
    std::condition_variable swap_cv;
    bool new_frame_ready = false;
    
    std::thread logic_thread;
    std::mutex scene_mutex;
    void logic_loop();
};
