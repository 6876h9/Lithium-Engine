#include "core/engine.hpp"
#include <typeinfo>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <limits>
#include "core/locosloco_png.h"
#include "world/static_mesh_component.hpp"
#include "world/animation_player.hpp"
#include "world/camera_component.hpp"
#include "world/character_controller_component.hpp"
#include "world/joint_component.hpp"
#include "world/ui_canvas_component.hpp"
#include "world/lod_group_component.hpp"
#include "world/nav_agent_component.hpp"
#include "world/terrain_component.hpp"
#include "renderer/lightmapper.hpp"
#include "navigation/navmesh.hpp"
#include "world/editor_primitive_actor.hpp"
#include "world/directional_light_actor.hpp"
#include "world/particle_emitter_component.hpp"
#include "core/mesh_resource.hpp"
#include "core/texture_resource.hpp"
#include "core/model_importer.hpp"
#include "world/spinning_cube_actor.hpp"
#include "world/pcg_spawner_actor.hpp"
#include "world/light_components.hpp"
#include "imgui_impl_sdl2.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <iostream>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "core/scene_serializer.hpp"
#include "core/input_map.hpp"
#include "world/static_slr_actor.hpp"
#include "portable-file-dialogs.h"
#include "core/resource_manager.hpp"
#include "audio/audio_engine.hpp"
#include "physics/physics_engine.hpp"
#include "network/network_manager.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

Engine::Engine() {
    window = std::make_unique<Window>("Lithium C++ Game Engine", 1280, 720);
    renderer = std::make_unique<Renderer>();
    task_graph = std::make_unique<Threading::TaskGraph>();
}

Engine::~Engine() {
    NetworkManager::get().shutdown();
    shutdown();
}

// Signals both threads to stop and wakes anything parked on swap_cv. Both the render
// and logic threads wait on that single condition variable, so clearing is_running
// without a notify leaves the logic thread asleep on an already-true predicate and
// logic_thread.join() hangs forever - which is what made closing the window freeze.
// --- Play In Editor state snapshot ------------------------------------------
//
// Entering Play used to serialise the scene to a file and Stop reloaded it. That
// round-trip is lossy: the scene format does not carry SLR volumes, point/spot lights,
// script components, or the newer material properties, so stopping silently destroyed
// anything the serialiser did not know about.
//
// Snapshotting in memory avoids the problem entirely - nothing is written, parsed or
// reconstructed. Holding a shared_ptr to every actor also keeps actors alive that the
// running game destroyed, so Stop can put them back exactly as they were.
void Engine::capture_pie_snapshot() {
    pie_actor_order = actors;          // keeps every actor alive for the duration
    pie_states.clear();
    pie_states.reserve(actors.size());
    for (auto& a : actors) {
        ActorPlayState st;
        st.actor = a.get();
        st.transform = a->get_actor_transform();
        st.color = a->actor_color;
        st.metallic = a->metallic;
        st.roughness = a->roughness;
        st.clearcoat = a->clearcoat;
        st.clearcoat_roughness = a->clearcoat_roughness;
        st.sheen = a->sheen;
        st.subsurface = a->subsurface;
        st.emissive = a->emissive;
        st.is_invisible = a->is_invisible;
        pie_states.push_back(st);
    }
}

void Engine::restore_pie_snapshot() {
    if (pie_actor_order.empty()) return;

    // Restore the exact actor set: anything the game spawned is dropped, and anything
    // it destroyed comes back, because the snapshot held a reference to it.
    actors = pie_actor_order;

    for (const ActorPlayState& st : pie_states) {
        Actor* a = st.actor;
        if (!a) continue;
        a->get_actor_transform() = st.transform;
        a->actor_color = st.color;
        a->metallic = st.metallic;
        a->roughness = st.roughness;
        a->clearcoat = st.clearcoat;
        a->clearcoat_roughness = st.clearcoat_roughness;
        a->sheen = st.sheen;
        a->subsurface = st.subsurface;
        a->emissive = st.emissive;
        a->is_invisible = st.is_invisible;
    }

    // Drop anything the game queued for destruction; those actors are being restored.
    actors_to_destroy.clear();

    pie_actor_order.clear();
    pie_states.clear();
}

// Brings the main window forward and asks for keyboard/mouse focus.
void Engine::focus_main_window() {
    SDL_Window* win = window->get_sdl_window();
    if (!win) return;
    SDL_RaiseWindow(win);
    SDL_SetWindowInputFocus(win);   // X11 only; harmless failure elsewhere
}

void Engine::request_shutdown() {
    {
        std::lock_guard<std::mutex> lock(swap_mutex);
        is_running = false;
    }
    swap_cv.notify_all();
}

// Loading indicator shown while the engine initialises. This is deliberately not a
// splash screen: there is no logo, no fade curves and no minimum hold. Loading is
// the only thing that keeps it on screen, so it disappears the instant startup
// finishes and the engine goes straight to the main menu.
//
// It cannot simply be deleted, though. Renderer startup compiles every shader and
// convolves the environment map - seconds of work during which nothing else pumps
// SDL. Without this the window stays black, the WM flags it as unresponsive, and
// closing the window is ignored until initialisation returns.

// Draws the loading contents into the current ImGui frame: a plain status line on
// the background colour, naming the phase currently running.
void Engine::draw_loading_ui() {
    glClearColor(0.10f, 0.11f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Loading", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);

    // Centre the status text. Measuring it rather than guessing keeps it centred as
    // the phase name changes length.
    std::string text = loading_status + "...";
    ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetCursorPos(ImVec2((display.x - text_size.x) * 0.5f, (display.y - text_size.y) * 0.5f));
    ImGui::TextUnformatted(text.c_str());

    ImGui::End();
    window->render_imgui();
}

// Presents one complete loading frame. Called between initialisation steps so the
// user sees progress rather than a black window.
void Engine::present_loading_frame() {
    window->new_frame();
    draw_loading_ui();
    window->swap_buffers();
}

// Services the window during long initialisation work: pumps events (so the window
// stays responsive and can be closed) and presents a frame. A non-null status
// updates the displayed phase; null means "still in the same phase", which is what
// the renderer's per-tick progress reports pass.
void Engine::pump_loading(const char* status) {
    if (status) loading_status = status;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT) {
            loading_quit_requested = true;
            return;
        }
    }

    // Events are pumped on every call so window close stays responsive, but
    // presenting is capped at ~60Hz. That lets initialisation code call back as
    // often as it likes without paying for a redundant full frame each time.
    Uint32 now_ms = SDL_GetTicks();
    if (loading_last_present_ms != 0 && (now_ms - loading_last_present_ms) < 16) return;
    loading_last_present_ms = now_ms;
    present_loading_frame();
}

bool Engine::initialize(const std::string& initial_scene_path, bool standalone) {
    if (!window->initialize()) {
        std::cerr << "Subsystem Window failed to initialize." << std::endl;
        return false;
    }

    // Bring ImGui up *before* the expensive subsystems, then present a frame straight
    // away. Renderer init alone compiles every shader and convolves the environment
    // map, which is seconds of work; doing it first meant the window sat black for
    // that whole time.
    SDL_RaiseWindow(window->get_sdl_window());

    load_engine_options();

    window->init_imgui();

    active_config.project_path = "/home/burair1991/projects";
    
    
    // Load window icon from memory. stbi_set_flip_vertically_on_load is a
    // global flag, not per-call - other loaders (e.g. the HDRI loader) set it
    // true for 3D texture sampling convention and don't reset it, so this must
    // explicitly set it false rather than relying on whatever was left behind.
    stbi_set_flip_vertically_on_load(false);
    int icon_width, icon_height, icon_channels;
    unsigned char* icon_data = stbi_load_from_memory(locosloco_png, locosloco_png_len, &icon_width, &icon_height, &icon_channels, 4);
    if (icon_data) {
        // Also create an OpenGL texture for the editor logo
        glGenTextures(1, &editor_logo_texture);
        glBindTexture(GL_TEXTURE_2D, editor_logo_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, icon_width, icon_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, icon_data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        if (icon_height > 0) {
            editor_ui.logo_aspect = static_cast<float>(icon_width) / static_cast<float>(icon_height);
        }

        SDL_Surface* icon_surface = SDL_CreateRGBSurfaceWithFormatFrom(
            icon_data, icon_width, icon_height, 32, icon_width * 4, SDL_PIXELFORMAT_RGBA32);
        if (icon_surface) {
            SDL_SetWindowIcon(window->get_sdl_window(), icon_surface);
            SDL_FreeSurface(icon_surface);
        }
        stbi_image_free(icon_data);
    }
    
    // Deterministic camera overrides for automated capture: without these the only
    // way to aim the camera is a mouse drag, which cannot be scripted.
    apply_engine_options();

    // Developer camera start, applied before the env overrides so a scripted run can
    // still take precedence.
    if (active_config.dev_camera_x != 0.0f || active_config.dev_camera_y != 0.0f || active_config.dev_camera_z != 0.0f) {
        camera_pos = { active_config.dev_camera_x, active_config.dev_camera_y, active_config.dev_camera_z };
    }
    if (active_config.dev_camera_yaw != 0.0f) camera_rot.y = active_config.dev_camera_yaw;
    if (active_config.dev_camera_pitch != 0.0f) camera_rot.x = active_config.dev_camera_pitch;

    if (const char* cx = std::getenv("LITHIUM_CAMERA_X")) camera_pos.x = std::atof(cx);
    if (const char* cz = std::getenv("LITHIUM_CAMERA_Z")) camera_pos.z = std::atof(cz);
    if (const char* yaw = std::getenv("LITHIUM_CAMERA_YAW")) camera_rot.y = static_cast<float>(std::atof(yaw));
    if (const char* pitch = std::getenv("LITHIUM_CAMERA_PITCH")) camera_rot.x = static_cast<float>(std::atof(pitch));

    startup_finished_time = SDL_GetTicks() / 1000.0f;
    std::cout << "[Startup] window and menus up at " << startup_finished_time
              << "s (subsystems deferred until a project is opened)" << std::endl;

    if (!initial_scene_path.empty()) {
        // Opening a scene straight from the command line skips the menus entirely,
        // so there is no project-browser click to defer the heavy work behind - it
        // has to happen here instead, behind the same loading screen.
        if (!initialize_runtime()) return false;
        if (standalone) {
            current_state = EngineState::PlayInEditor; // Play immediately
        } else {
            current_state = EngineState::Editor;
            editor_ui.current_scene_path = initial_scene_path;
        }
        SceneSerializer::load_scene(initial_scene_path, actors);
    } else {
        // Straight to the main menu, with nothing heavy loaded yet. The browser's
        // launch button is what triggers initialize_runtime().
        current_state = EngineState::MainMenu;
    }

    // The window can be closed while loading is still in flight.
    if (loading_quit_requested) return false;

    is_running = true;
    return true;
}

// Brings up everything expensive, driving the loading screen as it goes. Split out of
// initialize() so the main menu and project browser can be on screen within a fraction
// of a second: they are plain ImGui and need only a window and a GL context, whereas
// this is seconds of shader compilation and environment convolution.
bool Engine::initialize_runtime() {
    if (runtime_initialized) return true;

    float t_start = SDL_GetTicks() / 1000.0f;
    pump_loading("Starting up");

    // Renderer startup is the long pole (shader compilation plus environment map
    // convolution). Let it drive the loading frame so the window stays responsive
    // throughout and the status names the phase actually running.
    renderer->set_loading_callback([this](const char* status) { pump_loading(status); });
    bool renderer_ok = renderer->initialize(window->get_width(), window->get_height());
    renderer->set_loading_callback(nullptr);
    if (!renderer_ok) {
        std::cerr << "Subsystem Renderer failed to initialize." << std::endl;
        return false;
    }
    float t_renderer_done = SDL_GetTicks() / 1000.0f;

    pump_loading("Starting resource manager");
    ResourceManager::get().initialize(4); // 4 threads for asset loading
    pump_loading("Starting audio and physics");
    AudioEngine::get().init();
    PhysicsEngine::get_instance().initialize();
    pump_loading("Starting input");
    // Player-editable bindings live beside the engine config; fall back to the
    // built-in set the first time a project runs.
    if (!InputMap::get().load("input_bindings.json")) {
        InputMap::get().load_defaults();
    }
    InputMap::get().refresh_gamepad();

    pump_loading("Starting networking");
    if (!NetworkManager::get().initialize()) {
        std::cerr << "Subsystem NetworkManager failed to initialize." << std::endl;
        return false;
    }

    // Set up default camera View and Projection matrices
    Matrix4x4 view = Matrix4x4::translation({0.0f, 0.0f, -5.0f}); // Move camera back
    Matrix4x4 proj = Matrix4x4::perspective(45.0f, static_cast<float>(window->get_width()) / window->get_height(), 0.1f, 100.0f);
    renderer->set_view_matrix(view);
    renderer->set_projection_matrix(proj);

    runtime_initialized = true;

    // Re-apply the saved options now that the renderer and audio actually exist:
    // the copy run at startup could only take the window-level ones.
    apply_engine_options();

    float t_done = SDL_GetTicks() / 1000.0f;
    std::cout << "[Startup] renderer ready in " << (t_renderer_done - t_start)
              << "s, all subsystems ready in " << (t_done - t_start) << "s" << std::endl;

    // The window can be closed while loading is still in flight.
    if (loading_quit_requested) {
        is_running = false;
        return false;
    }
    return true;
}

void Engine::shutdown() {
    is_running = false;
    AudioEngine::get().shutdown();
    PhysicsEngine::get_instance().cleanup();
    actors.clear();
    if (renderer) renderer.reset();
    if (window) window.reset();
}

void Engine::run() {
    // Only call begin_play if we start in PlayInEditor mode
    if (current_state == EngineState::PlayInEditor) {
        for (auto& actor : actors) {
            actor->begin_play();
        }
    }

    logic_thread = std::thread(&Engine::logic_loop, this);


    float last_time = SDL_GetTicks() / 1000.0f;

    while (is_running) {
        if (pending_api_swap != RHI::BackendAPI::None) {
            std::cout << "Runtime API Swap Requested..." << std::endl;
            // Signal logic thread to stop temporarily
            bool was_running = is_running;
            is_running = false;
            swap_cv.notify_all();
            if (logic_thread.joinable()) {
                logic_thread.join();
            }

            // Invalidate GPU resources so they re-upload on the new context
            ResourceManager::get().invalidate_gpu_resources();

            // Destroy current context
            window->shutdown();
            
            // Re-initialize with new API
            RHI::RendererAPI::current_api = pending_api_swap.load();
            window->initialize();
            renderer->initialize(window->get_width(), window->get_height());
            window->init_imgui();

            // Save new config
            std::string config_path = "engine_config.json";
            nlohmann::json config_json;
            if (std::filesystem::exists(config_path)) {
                std::ifstream config_file(config_path);
                config_file >> config_json;
            }
            config_json["graphics_api"] = (RHI::RendererAPI::current_api == RHI::BackendAPI::Vulkan) ? "vulkan" : "opengl";
            std::ofstream out_file(config_path);
            out_file << config_json.dump(4);

            // Restart logic thread
            is_running = was_running;
            pending_api_swap = RHI::BackendAPI::None;
            if (is_running) {
                logic_thread = std::thread(&Engine::logic_loop, this);
            }
        }

        process_input();

        // Sampled once per frame and before scripts run, so every script in the frame
        // sees the same snapshot rather than each re-reading a moving keyboard state.
        if (runtime_initialized) InputMap::get().update();

        ResourceManager::get().update();

        // Wait for Logic thread to finish the frame
        {
            std::unique_lock<std::mutex> lock(swap_mutex);
            swap_cv.wait(lock, [this] { return new_frame_ready || !is_running; });
            
            if (!is_running) break;

            // Swap buffers
            std::swap(logic_buffer_index, render_buffer_index);
            new_frame_ready = false;
        }
        
        // Script-requested scene changes are applied here, on the main thread,
        // rather than on the logic thread that queued them. Spawning builds an
        // EditorPrimitiveActor, whose constructor uploads its geometry - and the GL
        // context is current only on this thread, so doing it anywhere else produces
        // actors that exist in the scene but have no vertex array and never draw.
        //
        // It runs while the logic thread is still parked on the swap, so the actor
        // list is not being touched concurrently.
        {
            std::lock_guard<std::mutex> lock(scene_mutex);
            apply_script_commands();
        }

        // Notify logic thread to start next frame. notify_all, not notify_one: the
        // render and logic threads both wait on this condition variable, so waking a
        // single arbitrary waiter can wake the wrong thread and stall the other.
        swap_cv.notify_all();

        float render_now = SDL_GetTicks() / 1000.0f;
        float render_delta_time = render_now - last_time;
        if (render_delta_time > 0.1f) render_delta_time = 0.1f; // guard against spikes (e.g. breakpoints)
        last_time = render_now;

        if (auto_screenshot_delay >= 0.0f) {
            auto_screenshot_elapsed += render_delta_time;
            if (auto_screenshot_elapsed >= auto_screenshot_delay) {
                screenshot_requested_path = auto_screenshot_path;
                screenshot_requested = true;
                auto_screenshot_delay = -1.0f; // fire once
            }
        }

        render(render_buffers[render_buffer_index], render_delta_time);
        window->swap_buffers();
    }

    // Wait for logic thread to exit before fully shutting down. Re-assert the stop
    // flag and wake every waiter first: the render loop can break out of its wait for
    // reasons other than SDL_QUIT, and joining a thread still parked on swap_cv would
    // hang shutdown indefinitely.
    {
        std::lock_guard<std::mutex> lock(swap_mutex);
        is_running = false;
    }
    swap_cv.notify_all();
    if (logic_thread.joinable()) {
        logic_thread.join();
    }

    // Final write on the way out, so settings changed anywhere in the session stick.
    save_engine_options();
}

void Engine::logic_loop() {
    float last_time = SDL_GetTicks() / 1000.0f;

    while (is_running) {
        float current_time = SDL_GetTicks() / 1000.0f;
        float delta_time = current_time - last_time;
        
        // Uncapped delta time, but prevent massive spikes if breakpoint hits
        if (delta_time > 0.1f) delta_time = 0.1f;
        
        last_time = current_time;

        {
            std::lock_guard<std::mutex> lock(scene_mutex);
            if (current_state == EngineState::PlayInEditor || current_state == EngineState::Editor || current_state == EngineState::PlayInEditorPaused) {
                update(delta_time);
            }
            destroy_queued_actors();
            populate_render_state(render_buffers[logic_buffer_index]);
        }

        // Wait for render thread to consume buffer
        {
            std::unique_lock<std::mutex> lock(swap_mutex);
            new_frame_ready = true;
            swap_cv.notify_all();
            swap_cv.wait(lock, [this] { return !new_frame_ready || !is_running; });
        }
    }
}

// Tangent of half the vertical field of view the scene is rendered with. The
// projection is built with a fixed 45 degree FOV; LOD thresholds are expressed as a
// fraction of screen height, and converting a world size into one needs this.
static constexpr float kProjectionTanHalfFov = 0.41421356f; // tan(22.5 degrees)

void Engine::populate_render_state(RenderState& state) {
    state.camera_pos = camera_pos;
    state.view_matrix = get_view_matrix();
    state.meshes.clear();
    state.particles.clear();
    state.terrains.clear();
    state.lights.clear();
    state.slr_volumes.clear();
    
    // Reset globals
    state.enable_taa = true;
    state.upscaling_scale = 1.0f;
    state.enable_ray_tracing = false;
    state.enable_embree = false;
    state.enable_3d_clouds = false;
    
    for (auto& actor : actors) {
        if (enable_world_partition && !is_in_active_chunk(actor->get_actor_transform().position)) {
            continue;
        }

        for (auto& comp : actor->get_components()) {
            if (auto mesh = dynamic_cast<StaticMeshComponent*>(comp.get())) {
                bool is_selected = editor_ui.is_actor_selected(actor.get());
                RenderMeshCommand cmd;
                cmd.actor = actor;
                cmd.mesh = mesh;
                cmd.transform = mesh->transform;
                if (auto* animator = mesh->get_animator()) {
                    cmd.bone_matrices = animator->get_bone_matrices();
                }

                // Bounds, resolved once and used by both level-of-detail
                // selection and the two culls on the render thread. Computed here
                // because reading them means touching the mesh resource, which the
                // render thread must not do while this thread may be swapping it.
                Vector3 bounds_min, bounds_max;
                if (mesh->get_local_bounds(bounds_min, bounds_max)) {
                    // A skinned mesh is bounded in its bind pose, and an animation
                    // routinely swings a limb outside that. Inflating rather than
                    // exempting it keeps culling working for characters without
                    // making them flicker at the screen edge.
                    if (!cmd.bone_matrices.empty()) {
                        const Vector3 centre = { (bounds_min.x + bounds_max.x) * 0.5f,
                                                 (bounds_min.y + bounds_max.y) * 0.5f,
                                                 (bounds_min.z + bounds_max.z) * 0.5f };
                        const float inflate = 1.5f;
                        bounds_min = centre + (bounds_min - centre) * inflate;
                        bounds_max = centre + (bounds_max - centre) * inflate;
                    }

                    const Matrix4x4 model = cmd.transform.get_matrix();
                    const Vector3 local_center = {
                        (bounds_min.x + bounds_max.x) * 0.5f,
                        (bounds_min.y + bounds_max.y) * 0.5f,
                        (bounds_min.z + bounds_max.z) * 0.5f
                    };
                    const Vector3 world_center = model * local_center;

                    // A non-uniform scale makes the bounding sphere the largest the
                    // box can be under any of the three, which over-covers rather
                    // than culling a stretched object that is still on screen.
                    const Vector3& scale = cmd.transform.scale;
                    const float max_scale = std::max(std::abs(scale.x),
                                            std::max(std::abs(scale.y), std::abs(scale.z)));
                    const Vector3 half = {
                        (bounds_max.x - bounds_min.x) * 0.5f,
                        (bounds_max.y - bounds_min.y) * 0.5f,
                        (bounds_max.z - bounds_min.z) * 0.5f
                    };
                    const float world_radius = half.length() * max_scale;

                    cmd.has_bounds = true;
                    cmd.bounds_center_world = world_center;
                    cmd.bounds_radius_world = world_radius;
                    cmd.bounds_local_min = bounds_min;
                    cmd.bounds_local_max = bounds_max;

                    // Level of detail. Resolving a level can start an asset load,
                    // which is another thing the render thread must not do.
                    if (auto* lod = actor->get_component<LODGroupComponent>()) {
                        const DVector3 to_camera = DVector3{ static_cast<double>(world_center.x),
                                                             static_cast<double>(world_center.y),
                                                             static_cast<double>(world_center.z) } - camera_pos;
                        const double distance = to_camera.length();
                        const float screen_height = LODGroupComponent::compute_screen_height(
                            world_radius, distance, kProjectionTanHalfFov);

                        const int level = lod->select_level(screen_height, distance);
                        lod->set_last_selected_level(level);
                        // Past the last level means the group asked for the object
                        // to stop being drawn entirely.
                        if (level >= static_cast<int>(lod->levels.size())) continue;
                        if (level >= 0) cmd.lod_mesh = lod->resource_for_level(level);
                    }
                }
                // Custom surface shader, if the actor's material names one. Compiled
                // once by the library and shared by every material using it.
                if (actor->assigned_material && !actor->assigned_material->shader_path.empty()) {
                    cmd.custom_shader =
                        MaterialShaderLibrary::get().load(actor->assigned_material->shader_path);
                    if (cmd.custom_shader) {
                        for (const auto& value : actor->assigned_material->shader_values) {
                            MaterialShader::Value resolved;
                            resolved.name = value.name;
                            resolved.number[0] = value.number[0];
                            resolved.number[1] = value.number[1];
                            resolved.number[2] = value.number[2];
                            resolved.texture_path = value.texture_path;
                            cmd.custom_shader_values.push_back(resolved);
                        }
                    }
                }

                // Indirect light for this object. A lightmapped actor ignores it -
                // its shader reads the atlas instead - but sampling anyway costs a
                // few multiplies and means the fallback is always populated.
                {
                    const Lightmapper::AmbientCube cube =
                        Lightmapper::get().sample_probes(cmd.transform.position);
                    for (int face = 0; face < 6; ++face) cmd.ambient_cube[face] = cube.axis[face];
                }

                cmd.color = actor->actor_color;
                cmd.metallic = actor->metallic;
                cmd.roughness = actor->roughness;
                cmd.clearcoat = actor->clearcoat;
                cmd.clearcoat_roughness = actor->clearcoat_roughness;
                cmd.sheen = actor->sheen;
                cmd.subsurface = actor->subsurface;
                // Baked bounce is added to emission because the lighting pass evaluates
                // it as albedo * emissive - the same albedo-modulated form indirect
                // diffuse light takes - and the G-buffer has no spare channel for a
                // separate term.
                cmd.emissive = actor->emissive +
                    (actor->has_baked_lighting ? actor->baked_irradiance : 0.0f);
                cmd.is_invisible = actor->is_invisible;
                cmd.is_selected = is_selected;
                state.meshes.push_back(cmd);
            } else if (auto terrain = dynamic_cast<TerrainComponent*>(comp.get())) {
                RenderTerrainCommand tcmd;
                tcmd.actor = actor;
                tcmd.terrain = terrain;
                // The actor's transform, not the component's: see
                // TerrainComponent::placement().
                tcmd.transform = actor->get_actor_transform();
                {
                    const Lightmapper::AmbientCube cube =
                        Lightmapper::get().sample_probes(tcmd.transform.position);
                    for (int face = 0; face < 6; ++face) tcmd.ambient_cube[face] = cube.axis[face];
                }
                state.terrains.push_back(tcmd);
            } else if (auto particles = dynamic_cast<ParticleEmitterComponent*>(comp.get())) {
                RenderParticleCommand pcmd;
                pcmd.actor = actor;
                pcmd.blend_mode = particles->blend_mode;
                pcmd.texture_path = particles->texture_path;

                // Resolved to world space here. A local-space emitter stores its
                // particles relative to itself, and only this thread may ask the
                // component where that is.
                const DVector3& emitter_origin = particles->transform.position;
                const bool local_space =
                    (particles->simulation_space == ParticleEmitterComponent::Space_Local);

                const auto& live = particles->get_particles();
                pcmd.instances.reserve(live.size());
                for (const Particle& p : live) {
                    const float age = ParticleEmitterComponent::particle_fraction(p);

                    Renderer::ParticleInstance instance;
                    instance.position = local_space
                        ? Vector3{ p.position.x + static_cast<float>(emitter_origin.x),
                                   p.position.y + static_cast<float>(emitter_origin.y),
                                   p.position.z + static_cast<float>(emitter_origin.z) }
                        : p.position;
                    instance.size = p.size * (particles->size_start_scale +
                        (particles->size_end_scale - particles->size_start_scale) * age);
                    instance.rotation = p.rotation;

                    const Vector3 tint = particles->start_color +
                        (particles->end_color - particles->start_color) * age;
                    const float alpha = particles->start_alpha +
                        (particles->end_alpha - particles->start_alpha) * age;
                    instance.color = { tint.x * particles->intensity,
                                       tint.y * particles->intensity,
                                       tint.z * particles->intensity,
                                       std::max(0.0f, alpha) };
                    pcmd.instances.push_back(instance);
                }

                // Alpha-blended particles have to be drawn far to near or the ones
                // in front punch holes in the ones behind. Additive is order
                // independent by construction, so it is left alone.
                if (pcmd.blend_mode == ParticleEmitterComponent::Blend_Alpha) {
                    const DVector3 eye = camera_pos;
                    std::sort(pcmd.instances.begin(), pcmd.instances.end(),
                              [&eye](const Renderer::ParticleInstance& a,
                                     const Renderer::ParticleInstance& b) {
                                  const double da = (DVector3{ a.position.x, a.position.y, a.position.z } - eye).length();
                                  const double db = (DVector3{ b.position.x, b.position.y, b.position.z } - eye).length();
                                  return da > db;
                              });
                }

                state.particles.push_back(pcmd);
            } else if (auto light = dynamic_cast<LightComponent*>(comp.get())) {
                RenderLightCommand lcmd;
                lcmd.actor = actor;
                lcmd.light = light;
                state.lights.push_back(lcmd);
            }
        }
        
        if (auto slr = dynamic_cast<StaticSLRActor*>(actor.get())) {
            RenderSLRCommand scmd;
            scmd.actor = actor;
            scmd.transform = actor->get_actor_transform();
            scmd.color = slr->slr_color;
            scmd.alpha = slr->slr_alpha;
            scmd.shape = slr->shape;
            scmd.sharpness = slr->sharpness;
            scmd.intensity = slr->intensity;
            scmd.falloff = slr->falloff;
            scmd.core = slr->core;
            state.slr_volumes.push_back(scmd);
        }

        if (auto d_light = dynamic_cast<DirectionalLightActor*>(actor.get())) {
            state.enable_taa = d_light->enable_taa;
            state.upscaling_scale = d_light->upscaling_scale;
            state.enable_ray_tracing = d_light->enable_ray_tracing;
            state.enable_embree = d_light->enable_embree;
            state.enable_3d_clouds = d_light->enable_3d_clouds;
            state.sky_mode = d_light->sky_mode;
            state.void_color = d_light->void_color;
        }
    }
}

// Input tracing, enabled with LITHIUM_DEBUG_INPUT=1. Cached rather than calling
// getenv per event - mouse motion arrives hundreds of times a second.
static bool input_debug_enabled() {
    static const bool enabled = (std::getenv("LITHIUM_DEBUG_INPUT") != nullptr);
    return enabled;
}

void Engine::process_input() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        std::lock_guard<std::mutex> lock(scene_mutex);
        
        // During an automated capture, keep UI input out entirely. Anything else on
        // the desktop that steals a click would otherwise land on the editor's
        // widgets mid-run and change the very values being captured.
        bool capture_in_progress = (auto_screenshot_delay >= 0.0f) && !auto_screenshot_path.empty();
        if (!capture_in_progress) {
            ImGui_ImplSDL2_ProcessEvent(&event);
        }
        if (event.type == SDL_DROPFILE) {
            // A file dragged from the desktop/file manager onto the window. SDL
            // allocates the path and transfers ownership, so it must be freed.
            if (event.drop.file) {
                pending_dropped_files.push_back(event.drop.file);
                SDL_free(event.drop.file);
            }
        }

        if (event.type == SDL_QUIT) {
            bool editing = (current_state == EngineState::Editor ||
                            current_state == EngineState::PlayInEditor ||
                            current_state == EngineState::PlayInEditorPaused);
            if (editing && editor_ui.has_unsaved_changes()) {
                // Don't tear down yet - ask first. The modal is drawn in render().
                pending_quit_prompt = true;
            } else {
                request_shutdown();
            }
        } else if (event.type == SDL_CONTROLLERDEVICEADDED ||
                   event.type == SDL_CONTROLLERDEVICEREMOVED) {
            // Pads get plugged in mid-session; without this the map keeps polling a
            // controller that is gone, or never notices a new one.
            InputMap::get().refresh_gamepad();
        } else if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                int w = event.window.data1;
                int h = event.window.data2;
                glViewport(0, 0, w, h);
                // The window is resizable from the menus, before the renderer has any
                // framebuffers to resize. initialize_runtime() sizes them correctly
                // from the current window dimensions when it does run.
                if (runtime_initialized) renderer->create_fbo(w, h);
            }
        }

        // Handle flight camera and focus inputs in Editor/PIE states.
        // While an automated screenshot is pending, ignore camera input entirely: a
        // stray click or drag from whatever else is on the display would otherwise
        // move the camera mid-run and make captures non-reproducible.
        bool capture_pending = (auto_screenshot_delay >= 0.0f) && !auto_screenshot_path.empty();
        // The editor's flight camera is an authoring tool, not a game camera. While
        // playing, the view belongs entirely to the running game - a first-person
        // controller, a follow camera, whatever the developer writes - so the editor
        // stops driving it. It stays available while paused, for inspecting the scene.
        bool editor_camera_active = (current_state == EngineState::Editor ||
                                     current_state == EngineState::PlayInEditorPaused);
        if (!capture_pending && editor_camera_active) {
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    is_rmb_down = true;
                    int rel_ok = SDL_SetRelativeMouseMode(SDL_TRUE);
                    if (input_debug_enabled())
                        std::cerr << "[input] RMB down, SDL_SetRelativeMouseMode=" << rel_ok
                                  << " (" << (rel_ok ? SDL_GetError() : "ok") << ")" << std::endl;
                }
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    is_rmb_down = false;
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                }
            } else if (event.type == SDL_MOUSEMOTION) {
                if (input_debug_enabled())
                    std::cerr << "[input] motion rel=(" << event.motion.xrel << "," << event.motion.yrel
                              << ") rmb=" << is_rmb_down
                              << " relmode=" << (SDL_GetRelativeMouseMode() ? 1 : 0)
                              << " yaw=" << camera_rot.y << std::endl;
                if (is_rmb_down) {
                    if (active_config.is_2d_mode) {
                        float pan_speed = ortho_zoom * 0.002f;
                        camera_pos.x -= event.motion.xrel * pan_speed;
                        camera_pos.y += event.motion.yrel * pan_speed;
                        camera_rot.x = 0; camera_rot.y = 0;
                    } else {
                        float sensitivity = 0.003f;
                        camera_rot.y -= event.motion.xrel * sensitivity;
                        camera_rot.x -= event.motion.yrel * sensitivity;

                        // Clamp pitch
                        if (camera_rot.x > 1.4f) camera_rot.x = 1.4f;
                        if (camera_rot.x < -1.4f) camera_rot.x = -1.4f;
                    }
                }
            } else if (event.type == SDL_MOUSEWHEEL) {
                // Scrolling a panel must not also dolly the viewport camera. ImGui
                // reports when the wheel belongs to a hovered/scrollable window, so
                // let it claim the event first.
                if (event.wheel.y != 0 && !ImGui::GetIO().WantCaptureMouse) {
                    if (active_config.is_2d_mode) {
                        ortho_zoom -= event.wheel.y * 1.0f;
                        if (ortho_zoom < 1.0f) ortho_zoom = 1.0f;
                    } else {
                        Matrix4x4 view_matrix = get_view_matrix();
                        Vector3 forward = { -view_matrix.m[2], -view_matrix.m[6], -view_matrix.m[10] };
                        float zoom_speed = 1.0f;
                        DVector3 d_forward = {static_cast<double>(forward.x), static_cast<double>(forward.y), static_cast<double>(forward.z)};
                        camera_pos += d_forward * (event.wheel.y * zoom_speed);
                    }
                }
            } else if (event.type == SDL_KEYDOWN && !ImGui::GetIO().WantCaptureKeyboard) {
                // Gated on WantCaptureKeyboard so editor shortcuts stay out of the way
                // while a panel widget has focus. Typing a value into a Details field
                // meant Ctrl+A selected every actor in the scene and Ctrl+D duplicated
                // the selection, instead of doing what those chords do in a text box.
                if (event.key.keysym.sym == SDLK_a && (event.key.keysym.mod & KMOD_CTRL)) {
                    editor_ui.clear_selection();
                    for (auto& actor : actors) {
                        editor_ui.select_actor(actor.get());
                    }
                } else if (event.key.keysym.sym == SDLK_a && (event.key.keysym.mod & KMOD_ALT)) {
                    editor_ui.clear_selection();
                } else if (event.key.keysym.sym == SDLK_d && (event.key.keysym.mod & KMOD_CTRL)) {
                    auto selected_actors = editor_ui.get_selected_actors();
                    std::vector<Actor*> newly_spawned;
                    for (Actor* s_actor : selected_actors) {
                        Actor* new_actor = nullptr;
                        if (auto prim = dynamic_cast<EditorPrimitiveActor*>(s_actor)) {
                            new_actor = spawn_actor<EditorPrimitiveActor>(prim->get_name() + "_Copy", prim->shape_type);
                        } else if (auto light = dynamic_cast<DirectionalLightActor*>(s_actor)) {
                            new_actor = spawn_actor<DirectionalLightActor>(light->get_name() + "_Copy");
                        } else if (auto spin = dynamic_cast<SpinningCubeActor*>(s_actor)) {
                            new_actor = spawn_actor<SpinningCubeActor>(spin->get_name() + "_Copy");
                        }
                        
                        if (new_actor) {
                            new_actor->get_actor_transform() = s_actor->get_actor_transform();
                            new_actor->is_invisible = s_actor->is_invisible;
                            new_actor->actor_color = s_actor->actor_color;
                            new_actor->metallic = s_actor->metallic;
                            new_actor->roughness = s_actor->roughness;
                            new_actor->clearcoat = s_actor->clearcoat;
                            new_actor->clearcoat_roughness = s_actor->clearcoat_roughness;
                            new_actor->sheen = s_actor->sheen;
                            new_actor->subsurface = s_actor->subsurface;
                            new_actor->shape_type = s_actor->shape_type;
                            
                            // Copy Light Components
                            if (auto pl = s_actor->get_component<PointLightComponent>()) {
                                auto new_pl = new_actor->create_component<PointLightComponent>(pl->get_name());
                                new_pl->color = pl->color; new_pl->intensity = pl->intensity; new_pl->radius = pl->radius;
                            } else if (auto sl = s_actor->get_component<SpotLightComponent>()) {
                                auto new_sl = new_actor->create_component<SpotLightComponent>(sl->get_name());
                                new_sl->color = sl->color; new_sl->intensity = sl->intensity; new_sl->inner_angle = sl->inner_angle; new_sl->outer_angle = sl->outer_angle;
                            } else if (auto al = s_actor->get_component<AreaLightComponent>()) {
                                auto new_al = new_actor->create_component<AreaLightComponent>(al->get_name());
                                new_al->color = al->color; new_al->intensity = al->intensity;
                            } else if (auto skl = s_actor->get_component<SkyLightComponent>()) {
                                auto new_skl = new_actor->create_component<SkyLightComponent>(skl->get_name());
                                new_skl->color = skl->color; new_skl->intensity = skl->intensity;
                            }
                            
                            new_actor->begin_play();
                            newly_spawned.push_back(new_actor);
                        }
                    }
                    if (!newly_spawned.empty()) {
                        editor_ui.clear_selection();
                        for (Actor* new_a : newly_spawned) {
                            editor_ui.select_actor(new_a);
                        }
                        editor_ui.gizmo_mode = 0; // Translate (Move) mode
                        editor_ui.record_scene_addition(newly_spawned);
                    }
                } else if (event.key.keysym.sym == SDLK_f) {
                    auto selected_actors = editor_ui.get_selected_actors();
                    if (!selected_actors.empty()) {
                        Actor* selected_actor = selected_actors[0];
                        DVector3 act_pos = selected_actor->get_actor_transform().position;
                        Matrix4x4 view_matrix = get_view_matrix();
                        Vector3 forward = { -view_matrix.m[2], -view_matrix.m[6], -view_matrix.m[10] };
                        DVector3 d_forward = {static_cast<double>(forward.x), static_cast<double>(forward.y), static_cast<double>(forward.z)};
                        camera_pos = act_pos - (d_forward * 3.0);
                    }
                } else if (event.key.keysym.sym == SDLK_F9) {
                    screenshot_requested = true;
                } else if (event.key.keysym.sym == SDLK_DELETE || event.key.keysym.sym == SDLK_BACKSPACE) {
                    // Delete selected actors. Recorded first: the command has to read
                    // their positions and take a shared_ptr reference while they are
                    // still in the list, otherwise erasing destroys them outright and
                    // there is nothing left for undo to restore.
                    std::vector<Actor*> selected = editor_ui.get_selected_actors();
                    editor_ui.record_scene_removal(selected);
                    for (Actor* s_actor : selected) {
                        for (auto it = actors.begin(); it != actors.end(); ) {
                            if (it->get() == s_actor) {
                                it = actors.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }
                    editor_ui.clear_selection();
                } else if (event.key.keysym.sym == SDLK_z && (SDL_GetModState() & KMOD_CTRL)) {
                    // Ctrl+Shift+Z redoes, matching the Ctrl+Y below.
                    if (SDL_GetModState() & KMOD_SHIFT) editor_ui.redo();
                    else editor_ui.undo();
                } else if (event.key.keysym.sym == SDLK_y && (SDL_GetModState() & KMOD_CTRL)) {
                    editor_ui.redo();
                } else if (event.key.keysym.sym == SDLK_w && (SDL_GetModState() & KMOD_CTRL)) {
                    editor_ui.gizmo_mode = 0; // Translate
                } else if (event.key.keysym.sym == SDLK_e && (SDL_GetModState() & KMOD_CTRL)) {
                    editor_ui.gizmo_mode = 1; // Rotate
                } else if (event.key.keysym.sym == SDLK_r && (SDL_GetModState() & KMOD_CTRL)) {
                    editor_ui.gizmo_mode = 2; // Scale
                } else if (event.key.keysym.sym == SDLK_s && (SDL_GetModState() & KMOD_CTRL)) {
                    editor_ui.gizmo_local_space = !editor_ui.gizmo_local_space; // Local/World Space
                }
            }
        }
    }
}

void Engine::update(float delta_time) {
    // Editor flight movement, suppressed while the game is running for the same
    // reason as the look controls above.
    if (is_rmb_down && current_state != EngineState::PlayInEditor) {
        const Uint8* state = SDL_GetKeyboardState(NULL);
        
        // Extract right and forward vectors directly from view matrix columns
        Matrix4x4 view = get_view_matrix();
        Vector3 right = { view.m[0], view.m[4], view.m[8] };
        Vector3 forward = { -view.m[2], -view.m[6], -view.m[10] };

        // Normalize vectors to ensure constant speed
        float r_len = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
        if (r_len > 0.0f) {
            right.x /= r_len; right.y /= r_len; right.z /= r_len;
        }
        float f_len = std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
        if (f_len > 0.0f) {
            forward.x /= f_len; forward.y /= f_len; forward.z /= f_len;
        }

        float speed = 4.0f * delta_time;

        if (state[SDL_SCANCODE_LSHIFT] || state[SDL_SCANCODE_RSHIFT]) {
            speed *= 3.0f;
        }
        
        DVector3 d_forward = {static_cast<double>(forward.x), static_cast<double>(forward.y), static_cast<double>(forward.z)};
        DVector3 d_right = {static_cast<double>(right.x), static_cast<double>(right.y), static_cast<double>(right.z)};

        if (active_config.is_2d_mode) {
            float pan_speed = ortho_zoom * speed * 0.1f;
            if (state[SDL_SCANCODE_W]) camera_pos.y += pan_speed;
            if (state[SDL_SCANCODE_S]) camera_pos.y -= pan_speed;
            if (state[SDL_SCANCODE_A]) camera_pos.x -= pan_speed;
            if (state[SDL_SCANCODE_D]) camera_pos.x += pan_speed;
        } else {
            if (state[SDL_SCANCODE_W]) camera_pos += d_forward * speed;
            if (state[SDL_SCANCODE_S]) camera_pos -= d_forward * speed;
            if (state[SDL_SCANCODE_A]) camera_pos -= d_right * speed;
            if (state[SDL_SCANCODE_D]) camera_pos += d_right * speed;
            if (state[SDL_SCANCODE_SPACE]) camera_pos.y += speed;
            if (state[SDL_SCANCODE_Q]) camera_pos.y -= speed;
        }
    }

    NetworkManager::get().update();

    // Skeletal animation. Advanced here rather than from component tick() so a clip
    // plays in the editor viewport too - tick() only runs in PlayInEditor, which
    // would leave every imported character standing in bind pose while authoring.
    // Paused is excluded so stepping through a scene freezes the pose with it.
    if (current_state != EngineState::PlayInEditorPaused) {
        for (auto& actor : actors) {
            if (!actor) continue;
            for (auto& comp : actor->get_components()) {
                if (auto mesh = dynamic_cast<StaticMeshComponent*>(comp.get())) {
                    mesh->update_animation(delta_time);
                }
            }
        }
    }

    if (current_state == EngineState::PlayInEditor) {
        // Joints, before the step so a motor change made this frame is applied by
        // it. Sequential for the same reason character controllers are: building a
        // constraint calls into PhysicsSystem and locks both bodies, and the actor
        // tick runs across a task graph where two of those could overlap.
        for (auto& actor : actors) {
            if (!actor) continue;
            for (auto& comp : actor->get_components()) {
                if (auto* joint = dynamic_cast<JointComponent*>(comp.get())) {
                    joint->update_joint(delta_time);
                }
            }
        }

        PhysicsEngine::get_instance().tick(delta_time);

        // Physics contacts, resolved into per-pair Enter/Stay/Exit and delivered
        // here on the logic thread. Jolt raises them on its worker threads with all
        // bodies locked, where touching the scene or entering a script VM would be
        // unsafe, so nothing is dispatched until the step has fully finished.
        for (const PhysicsContactEvent& event : PhysicsEngine::get_instance().collect_contact_events()) {
            Actor* actor_a = PhysicsEngine::get_instance().actor_for_body(event.body_a);
            Actor* actor_b = PhysicsEngine::get_instance().actor_for_body(event.body_b);
            if (!actor_a && !actor_b) continue;

            if (event.is_trigger) {
                // Both sides are told, so either the trigger volume or the thing
                // entering it can hold the logic.
                if (actor_a) {
                    switch (event.phase) {
                        case PhysicsContactEvent::Phase::Enter: actor_a->dispatch_trigger_enter(actor_b); break;
                        case PhysicsContactEvent::Phase::Stay:  actor_a->dispatch_trigger_stay(actor_b);  break;
                        case PhysicsContactEvent::Phase::Exit:  actor_a->dispatch_trigger_exit(actor_b);  break;
                    }
                }
                if (actor_b) {
                    switch (event.phase) {
                        case PhysicsContactEvent::Phase::Enter: actor_b->dispatch_trigger_enter(actor_a); break;
                        case PhysicsContactEvent::Phase::Stay:  actor_b->dispatch_trigger_stay(actor_a);  break;
                        case PhysicsContactEvent::Phase::Exit:  actor_b->dispatch_trigger_exit(actor_a);  break;
                    }
                }
                continue;
            }

            // The normal is flipped for the second actor so that each side is told
            // the direction to push away along, rather than one of them receiving a
            // normal that points into itself.
            CollisionInfo info_for_a;
            info_for_a.other = actor_b;
            info_for_a.point = event.point;
            info_for_a.normal = event.normal;
            info_for_a.approach_speed = event.approach_speed;

            CollisionInfo info_for_b = info_for_a;
            info_for_b.other = actor_a;
            info_for_b.normal = { -event.normal.x, -event.normal.y, -event.normal.z };

            if (actor_a) {
                switch (event.phase) {
                    case PhysicsContactEvent::Phase::Enter: actor_a->dispatch_collision_enter(info_for_a); break;
                    case PhysicsContactEvent::Phase::Stay:  actor_a->dispatch_collision_stay(info_for_a);  break;
                    case PhysicsContactEvent::Phase::Exit:  actor_a->dispatch_collision_exit(info_for_a);  break;
                }
            }
            if (actor_b) {
                switch (event.phase) {
                    case PhysicsContactEvent::Phase::Enter: actor_b->dispatch_collision_enter(info_for_b); break;
                    case PhysicsContactEvent::Phase::Stay:  actor_b->dispatch_collision_stay(info_for_b);  break;
                    case PhysicsContactEvent::Phase::Exit:  actor_b->dispatch_collision_exit(info_for_b);  break;
                }
            }
        }

        // Navigation agents, stepped before the character controllers so the
        // movement intent an agent produces is consumed by the same frame's
        // character step rather than lagging one behind it. Sequential for the same
        // reason: an agent driving a character reaches Jolt's shared temp allocator.
        for (auto& actor : actors) {
            if (!actor) continue;
            for (auto& comp : actor->get_components()) {
                if (auto* agent = dynamic_cast<NavAgentComponent*>(comp.get())) {
                    agent->update_agent(delta_time);
                }
            }
        }

        // Character controllers, stepped after the physics solve so they collide
        // against this frame's world, and before the camera is placed so a
        // first-person view does not trail the body it is attached to by a frame.
        //
        // Sequential on purpose. Jolt's temp allocator is a linear allocator shared
        // by every collision query, so two characters sweeping at once from the
        // parallel actor tick would corrupt it.
        for (auto& actor : actors) {
            if (!actor) continue;
            for (auto& comp : actor->get_components()) {
                if (auto* controller = dynamic_cast<CharacterControllerComponent*>(comp.get())) {
                    controller->update_character(delta_time);
                }
            }
        }

        // Find active camera component in scene and override camera view
        for (auto& actor : actors) {
            if (!actor) continue;
            if (auto cam = actor->get_component<CameraComponent>()) {
                if (cam->is_active) {
                    camera_pos = actor->get_actor_transform().position + cam->transform.position;
                    camera_rot = actor->get_actor_transform().rotation + cam->transform.rotation;
                    break;
                }
            }
        }
        
        // Sync incoming transforms if client
        if (NetworkManager::get().get_mode() == NetworkMode::Client) {
            const auto& transforms = NetworkManager::get().get_received_transforms();
            for (const auto& t : transforms) {
                if (t.actor_id < actors.size()) {
                    actors[t.actor_id]->get_actor_transform().position = {static_cast<double>(t.position.x), static_cast<double>(t.position.y), static_cast<double>(t.position.z)};
                    actors[t.actor_id]->get_actor_transform().rotation = t.rotation;
                    actors[t.actor_id]->get_actor_transform().scale = t.scale;
                }
            }
            NetworkManager::get().clear_received_transforms();
        }

        // Integrate script-driven motion. Only while actually playing: in the editor
        // a non-zero velocity must not make objects drift away while you are working.
        if (current_state == EngineState::PlayInEditor) {
            for (auto& actor : actors) {
                if (!actor) continue;
                const Vector3& v = actor->velocity;
                const Vector3& av = actor->angular_velocity;
                if (v.x != 0.0f || v.y != 0.0f || v.z != 0.0f) {
                    Transform& t = actor->get_actor_transform();
                    t.position.x += static_cast<double>(v.x) * delta_time;
                    t.position.y += static_cast<double>(v.y) * delta_time;
                    t.position.z += static_cast<double>(v.z) * delta_time;
                }
                if (av.x != 0.0f || av.y != 0.0f || av.z != 0.0f) {
                    Transform& t = actor->get_actor_transform();
                    t.rotation.x += av.x * delta_time;
                    t.rotation.y += av.y * delta_time;
                    t.rotation.z += av.z * delta_time;
                }
            }
        }

        uint32_t index = 0;
        std::vector<std::future<void>> actor_tasks;
        for (auto& actor : actors) {
            if (auto pcg_actor = dynamic_cast<PCGSpawnerActor*>(actor.get())) {
                pcg_actor->generate_around_camera(camera_pos);
            }

            // World Partition Culling
            if (enable_world_partition && !is_in_active_chunk(actor->get_actor_transform().position)) {
                index++;
                continue;
            }

            // Dispatch tick to the task graph
            actor_tasks.push_back(task_graph->dispatch([&actor, delta_time]() {
                actor->tick(delta_time);
            }));
            
            // Broadcast if Server (Needs to stay on main thread for network consistency, or protected by mutex, but keeping here for simplicity)
            if (NetworkManager::get().get_mode() == NetworkMode::Server) {
                NetworkManager::get().broadcast_transform(
                    index, 
                    actor->get_actor_transform().position.to_vec3(), 
                    actor->get_actor_transform().rotation, 
                    actor->get_actor_transform().scale
                );
            }
            index++;
        }

        // Wait for all actors to finish ticking
        for (auto& fut : actor_tasks) {
            fut.get();
        }
    }
}

Matrix4x4 Engine::get_view_matrix() const {
    Matrix4x4 rotX = Matrix4x4::rotationX(camera_rot.x);
    Matrix4x4 rotY = Matrix4x4::rotationY(camera_rot.y);
    Matrix4x4 rotation = rotX * rotY; // Apply Yaw first, then Pitch to fix roll bug
    // LWC: Translation is now handled in get_relative_matrix on a per-actor basis. 
    // The view matrix is purely rotational.
    return rotation;
}

bool Engine::is_in_active_chunk(const DVector3& pos) const {
    int cam_cx = static_cast<int>(std::floor(camera_pos.x / chunk_size));
    int cam_cz = static_cast<int>(std::floor(camera_pos.z / chunk_size));

    int actor_cx = static_cast<int>(std::floor(pos.x / chunk_size));
    int actor_cz = static_cast<int>(std::floor(pos.z / chunk_size));

    // Active if within 3x3 chunks around camera (distance of 1 in chunk space)
    return std::abs(cam_cx - actor_cx) <= 1 && std::abs(cam_cz - actor_cz) <= 1;
}

void Engine::load_project_template() {
    actors.clear();
    // Templates may override renderer-wide settings (fog, environment map), so reset
    // them here rather than letting one template's choices leak into the next.
    // Lighter default haze. At 0.018 the fog lifted the shadows and flattened the
    // distance in ordinary outdoor scenes.
    if (renderer) renderer->fog_density = 0.009f;

    std::cout << "\n--------------------------------------------------" << std::endl;
    std::cout << "Creating Project: " << active_config.project_name << std::endl;
    std::cout << "Target Directory: " << active_config.project_path << std::endl;
    std::cout << "Hardware Platform: " << active_config.target_platform << std::endl;
    std::cout << "Physics Tickrate: " << active_config.physics_hz << "Hz" << std::endl;
    std::cout << "VSync: " << (active_config.vsync ? "Enabled" : "Disabled") << std::endl;
    std::cout << "Ray Tracing / Path Tracing Simulation: " << (active_config.enable_raytracing ? "Enabled" : "Disabled") << std::endl;
    std::cout << "DLSS / FSR Super Resolution: " << (active_config.enable_dlss_fsr ? "Enabled" : "Disabled") << std::endl;
    std::cout << "UE4 High-Fidelity Lighting Shader: " << (active_config.enable_ue4_lighting ? "Enabled" : "Disabled") << std::endl;
    std::cout << "--------------------------------------------------\n" << std::endl;

    // Set user-defined VSync
    SDL_GL_SetSwapInterval(active_config.vsync ? 1 : 0);

    // Spawn Sun Light for the scene
    auto sun = spawn_actor<DirectionalLightActor>("Sun");
    // Sunlight should clearly dominate sky ambient on lit surfaces - at 1.5 the
    // HDRI's diffuse irradiance was roughly twice the sun's contribution, which is
    // why the default scene looked flat and shapeless. (The earlier "blowout" that
    // prompted lowering this was the environment map's Inf contamination, not the
    // light being genuinely too strong; auto-exposure handles the absolute level.)
    // 5.0 pushed the whole frame into a narrow bright band: with a daylight HDRI
    // already supplying strong ambient, the extra direct light left nothing dark to
    // contrast against and the scene read as washed out rather than sunny.
    sun->light_comp->intensity = 3.0f;
    sun->get_actor_transform().rotation = { 45.0f * (3.14159265f / 180.0f), 30.0f * (3.14159265f / 180.0f), 0.0f };

    // Apply template specific actor setups
    if (active_config.selected_template == EngineTemplate::FPS) {
        std::cout << "Loading [FPS Shooter Template] Actors..." << std::endl;
        
        auto target = spawn_actor<EditorPrimitiveActor>("FPSTarget", "Sphere");
        target->actor_color = { 0.2f, 0.8f, 0.2f };
        target->get_actor_transform().position = { 0.0f, 0.0f, -4.5f };

        auto gun = spawn_actor<EditorPrimitiveActor>("FPSGun", "Oval");
        gun->actor_color = { 0.1f, 0.4f, 0.8f };
        gun->get_actor_transform().position = { 0.6f, -0.5f, -2.5f };
        gun->get_actor_transform().scale = { 0.15f, 0.15f, 0.8f };
    } else if (active_config.selected_template == EngineTemplate::ThirdPerson) {
        std::cout << "Loading [Third-Person Action Template] Actors..." << std::endl;

        auto ground = spawn_actor<EditorPrimitiveActor>("GroundPlane", "Square");
        ground->actor_color = { 0.25f, 0.25f, 0.25f };
        ground->get_actor_transform().position = { 0.0f, -1.0f, -4.5f };
        ground->get_actor_transform().scale = { 4.0f, 1.0f, 4.0f };

        auto character = spawn_actor<EditorPrimitiveActor>("PlayerCharacter", "Cube");
        character->actor_color = { 0.9f, 0.15f, 0.15f };
        character->get_actor_transform().position = { 0.0f, -0.4f, -4.5f };
        character->get_actor_transform().scale = { 0.6f, 1.0f, 0.6f };
    } else if (active_config.selected_template == EngineTemplate::Platformer) {
        std::cout << "Loading [2D Side Scroller Template] Actors..." << std::endl;

        auto player = spawn_actor<EditorPrimitiveActor>("2DPlayer", "Sphere");
        player->actor_color = { 1.0f, 0.75f, 0.0f };
        player->get_actor_transform().position = { -1.2f, -0.3f, -4.0f };
        player->get_actor_transform().scale = { 0.4f, 0.4f, 0.4f };

        auto ground = spawn_actor<EditorPrimitiveActor>("GroundBlock", "Cube");
        ground->actor_color = { 0.4f, 0.4f, 0.4f };
        ground->get_actor_transform().position = { 0.0f, -0.8f, -4.0f };
        ground->get_actor_transform().scale = { 4.0f, 0.2f, 1.0f };
    } else if (active_config.selected_template == EngineTemplate::SSRShowcase) {
        std::cout << "Loading [Reflections Showcase] Actors..." << std::endl;

        // Night interior lit through a window. The environment map is swapped for a
        // CC0 night sky so what the window looks out onto and what lights the room
        // are the same source.
        // The night HDRI still drives image-based ambient light, but the visible
        // background is a flat void colour (the engine default), so the window looks
        // out onto clean darkness rather than a busy photographic sky.
        if (renderer) {
            renderer->load_environment_map("EngineContent/NightSky.hdr");
            // Aerial perspective is an outdoor effect: over the few metres of an
            // interior it just lays a milky haze over everything and washes the walls
            // out. Distances in here are far too short for it to mean anything.
            renderer->fog_density = 0.0f;
        }
        sun->sky_mode = 2;
        sun->void_color = { 0.012f, 0.016f, 0.035f };
        sun->light_comp->intensity = 0.12f;
        sun->light_comp->color = { 0.45f, 0.55f, 0.85f };   // cool moonlight through the glass
        sun->get_actor_transform().rotation = { 28.0f * (3.14159265f / 180.0f), -75.0f * (3.14159265f / 180.0f), 0.0f };

        // Room dimensions. generate_cube spans +/-0.5 and generate_square spans
        // +/-0.5 in X/Z, so an actor's scale is its full size, not a half-extent -
        // the first pass treated it as a half-extent, which is why the floor came out
        // a quarter of the size of the walls surrounding it.
        const float room_w = 10.0f;   // interior width  (X): -5 .. +5
        const float room_d = 10.0f;   // interior depth  (Z)
        const float wall_h = 3.2f;
        const float wall_t = 0.25f;
        const float floor_y = -1.0f;
        const float cz = -3.0f;       // room centre, chosen so the default camera
                                      // (0,0,0) starts inside the room looking in
        const float hx = room_w * 0.5f;
        const float hz = room_d * 0.5f;
        const float wall_cy = floor_y + wall_h * 0.5f;
        const float ceil_y = floor_y + wall_h;

        // Polished wood floor. SSR keys off smoothness (1 - roughness), so keeping
        // roughness low is what makes the room actually mirror.
        auto floor = spawn_actor<EditorPrimitiveActor>("WoodFloor", "Square");
        floor->actor_color = { 0.70f, 0.52f, 0.33f };
        floor->metallic = 0.0f;
        floor->roughness = 0.16f;
        floor->get_actor_transform().position = { 0.0f, floor_y, cz };
        floor->get_actor_transform().scale = { room_w, 1.0f, room_d };
        if (auto* fm = floor->get_component<StaticMeshComponent>()) {
            fm->set_diffuse_texture(ResourceManager::get().load_async<TextureResource>("Content/Textures/WoodFloor.jpg"));
        }

        auto ceiling = spawn_actor<EditorPrimitiveActor>("Ceiling", "Square");
        ceiling->actor_color = { 0.14f, 0.15f, 0.14f };
        ceiling->roughness = 0.9f;
        ceiling->get_actor_transform().position = { 0.0f, ceil_y, cz };
        ceiling->get_actor_transform().scale = { room_w, 1.0f, room_d };

        const Vector3 wall_green = { 0.07f, 0.17f, 0.11f };   // dark green paint
        auto make_wall = [&](const char* name, Vector3 pos, Vector3 scale) {
            auto w = spawn_actor<EditorPrimitiveActor>(name, "Cube");
            w->actor_color = wall_green;
            w->metallic = 0.0f;
            w->roughness = 0.6f;
            w->get_actor_transform().position = { pos.x, pos.y, pos.z };
            w->get_actor_transform().scale = scale;
            return w;
        };

        make_wall("WallLeft",  { -hx - wall_t * 0.5f, wall_cy, cz },  { wall_t, wall_h, room_d + wall_t * 2.0f });
        make_wall("WallBack",  { 0.0f, wall_cy, cz - hz - wall_t * 0.5f }, { room_w + wall_t * 2.0f, wall_h, wall_t });
        make_wall("WallFront", { 0.0f, wall_cy, cz + hz + wall_t * 0.5f }, { room_w + wall_t * 2.0f, wall_h, wall_t });

        // Right wall, split into four pieces around a real window opening so the sky
        // and moonlight come through an actual gap rather than a painted rectangle.
        const float win_y0 = floor_y + 0.7f;   // sill
        const float win_y1 = floor_y + 2.3f;   // head
        const float win_z0 = cz - 1.8f;
        const float win_z1 = cz + 1.8f;
        const float rx = hx + wall_t * 0.5f;

        make_wall("WallRight_Sill",   { rx, (floor_y + win_y0) * 0.5f, cz },
                  { wall_t, win_y0 - floor_y, room_d + wall_t * 2.0f });
        make_wall("WallRight_Header", { rx, (win_y1 + ceil_y) * 0.5f, cz },
                  { wall_t, ceil_y - win_y1, room_d + wall_t * 2.0f });
        float pier_depth = (hz - 1.8f);
        make_wall("WallRight_PierBack",  { rx, (win_y0 + win_y1) * 0.5f, (cz - hz + win_z0) * 0.5f },
                  { wall_t, win_y1 - win_y0, pier_depth });
        make_wall("WallRight_PierFront", { rx, (win_y0 + win_y1) * 0.5f, (cz + hz + win_z1) * 0.5f },
                  { wall_t, win_y1 - win_y0, pier_depth });

        // Sofa: CC0 model from Poly Haven, imported through assimp on first run and
        // cached as a .mesh so later launches skip the conversion.
        {
            const std::string mesh_out = "Content/Sofa_01.mesh";
            if (!std::filesystem::exists(mesh_out) && std::filesystem::exists("Content/Sofa_01.gltf")) {
                std::cout << "[Content] Converting Sofa_01.gltf -> .mesh (first run only)..." << std::endl;
                ModelImporter::import_model("Content/Sofa_01.gltf");
            }
            if (std::filesystem::exists(mesh_out)) {
                auto sofa = spawn_actor<Actor>("Sofa");
                sofa->shape_type = "StaticMesh";
                sofa->mesh_path = mesh_out;
                sofa->metallic = 0.0f;
                sofa->roughness = 0.75f;
                auto* sm = sofa->create_component<StaticMeshComponent>("Mesh");
                sm->set_mesh_resource(ResourceManager::get().load_async<MeshResource>(sofa->mesh_path));
                sofa->set_root_component(sm);
                // The model is authored in metres and the engine's units are metres,
                // so it needs no rescaling. Backed against the rear wall, facing the camera.
                sofa->get_actor_transform().position = { -1.9f, floor_y, cz - hz + 1.1f };
                sofa->get_actor_transform().rotation = { 0.0f, 0.0f, 0.0f };
                sofa->get_actor_transform().scale = { 1.0f, 1.0f, 1.0f };
            } else {
                std::cerr << "[Content] Sofa model unavailable; skipping." << std::endl;
            }
        }

        // Chrome cube - the reflection subject, sitting on the floor.
        auto cube = spawn_actor<EditorPrimitiveActor>("ChromeCube", "Cube");
        cube->actor_color = { 0.87f, 0.88f, 0.92f };
        cube->metallic = 1.0f;
        cube->roughness = 0.11f;
        cube->get_actor_transform().position = { 1.9f, floor_y + 0.45f, cz - 0.4f };
        cube->get_actor_transform().scale = { 0.9f, 0.9f, 0.9f };

        // Hanging bulb: emissive sphere plus a point light, so the source is both
        // visible and actually lighting the room.
        //
        // Emissive is kept low deliberately. Pushed high the sphere clips to a flat
        // white disc and blooms into a blob - the shape stops being readable, so you
        // see a glow but not a bulb. The point light below does the illuminating;
        // the emissive only needs to make the glass read as lit.
        auto flex = spawn_actor<EditorPrimitiveActor>("BulbCord", "Cube");
        flex->actor_color = { 0.05f, 0.05f, 0.05f };
        flex->roughness = 0.9f;
        flex->get_actor_transform().position = { 0.0f, ceil_y - 0.22f, cz - 0.5f };
        flex->get_actor_transform().scale = { 0.03f, 0.44f, 0.03f };

        auto bulb = spawn_actor<EditorPrimitiveActor>("LightBulb", "Sphere");
        bulb->actor_color = { 1.0f, 0.88f, 0.66f };
        bulb->roughness = 0.25f;
        bulb->emissive = 1.15f;
        // Hung just under the ceiling on the cord above, rather than floating at
        // mid-wall height where the glow read as coming from nowhere.
        bulb->get_actor_transform().position = { 0.0f, ceil_y - 0.52f, cz - 0.5f };
        bulb->get_actor_transform().scale = { 0.24f, 0.24f, 0.24f };
        auto* bulb_light = bulb->create_component<PointLightComponent>("BulbLight");
        bulb_light->color = { 1.0f, 0.84f, 0.58f };
        // Point lights fall off as 1/d^2, so this is tuned for a room a few metres
        // across rather than being cranked until something is visible.
        bulb_light->intensity = 11.0f;
        bulb_light->radius = 18.0f;


    } else {
        std::cout << "Loading [Blank Template] Setup..." << std::endl;
        
        auto floor = spawn_actor<EditorPrimitiveActor>("FloorPlane", "Square");
        floor->actor_color = { 0.15f, 0.15f, 0.15f }; // Slightly darker grey for grid
        floor->metallic = 0.0f;
        floor->roughness = 0.9f;
        floor->get_actor_transform().position = { 0.0f, -1.0f, -5.0f };
        floor->get_actor_transform().scale = { 20.0f, 0.5f, 20.0f };

        // Real PBR-textured sample model as the centerpiece, so a brand new
        // project shows off the full asset pipeline (imported mesh + texture)
        // alongside the renderer's lighting, not just bare colored primitives.
        auto helmet = spawn_actor<Actor>("DamagedHelmet");
        helmet->shape_type = "StaticMesh";
        helmet->mesh_path = "Content/DamagedHelmet.mesh";
        helmet->metallic = 0.05f;
        helmet->roughness = 0.65f;
        auto helmet_mesh_comp = helmet->create_component<StaticMeshComponent>("Mesh");
        helmet_mesh_comp->set_mesh_resource(ResourceManager::get().load_async<MeshResource>(helmet->mesh_path));
        helmet->set_root_component(helmet_mesh_comp);
        helmet->get_actor_transform().position = { 0.0f, 0.1f, -5.0f };
        helmet->get_actor_transform().rotation = { 0.0f, 0.5f, 0.0f };
        helmet->get_actor_transform().scale = { 1.4f, 1.4f, 1.4f };

        auto red_cube = spawn_actor<EditorPrimitiveActor>("RedCube", "Cube");
        red_cube->actor_color = { 0.8f, 0.3f, 0.3f };
        red_cube->metallic = 0.85f;
        red_cube->roughness = 0.25f; // polished metal
        red_cube->get_actor_transform().position = { 1.5f, -0.65f, -5.0f };
        red_cube->get_actor_transform().scale = { 0.7f, 0.7f, 0.7f };

        auto blue_sphere = spawn_actor<EditorPrimitiveActor>("BlueSphere", "Sphere");
        blue_sphere->actor_color = { 0.3f, 0.5f, 0.8f };
        blue_sphere->metallic = 0.05f;
        blue_sphere->roughness = 0.1f; // glossy dielectric
        blue_sphere->get_actor_transform().position = { -1.5f, -0.5f, -6.0f };

        auto clearcoat_sphere = spawn_actor<EditorPrimitiveActor>("ClearcoatSphere", "Sphere");
        clearcoat_sphere->actor_color = { 0.05f, 0.2f, 0.5f };
        clearcoat_sphere->metallic = 0.6f;
        clearcoat_sphere->roughness = 0.35f;
        clearcoat_sphere->clearcoat = 1.0f;
        clearcoat_sphere->clearcoat_roughness = 0.05f; // car-paint style lacquer coat
        clearcoat_sphere->get_actor_transform().position = { 3.0f, -0.5f, -5.0f };
        clearcoat_sphere->get_actor_transform().scale = { 0.7f, 0.7f, 0.7f };
    }
}

// ---------------------------------------------------------------------------
// TESLA scene translation
//
// Turns a frame's render state into the path tracer's scene. Two things the old
// translation dropped are restored here: per-material emission (it was hardcoded to
// "1.0 if this actor happens to be the sun gizmo, else 0", so no material in a scene
// could ever emit light), and large-world precision (geometry was baked in absolute
// float coordinates, opting out of the LWC system the raster path uses).
// ---------------------------------------------------------------------------
namespace {

inline void hash_bytes(uint64_t& h, const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
}
template <typename T>
inline void hash_value(uint64_t& h, const T& v) { hash_bytes(h, &v, sizeof(T)); }

// Everything TESLA reads out of the render state. If this is unchanged, the BVH is
// still valid and rebuilding it would only throw away accumulated samples.
uint64_t tesla_scene_signature(const RenderState& state) {
    uint64_t h = 1469598103934665603ULL;
    for (const auto& cmd : state.meshes) {
        if (cmd.is_invisible) continue;
        hash_value(h, cmd.mesh);
        hash_value(h, cmd.transform.position);
        hash_value(h, cmd.transform.rotation);
        hash_value(h, cmd.transform.scale);
        hash_value(h, cmd.color);
        hash_value(h, cmd.metallic);
        hash_value(h, cmd.roughness);
        hash_value(h, cmd.emissive);
        // A mesh's texture arrives asynchronously, so the scene has to be rebuilt
        // when it lands - otherwise the first build (untextured) is the one that
        // sticks and the surface never picks the map up.
        if (cmd.mesh) {
            cmd.mesh->ensure_diffuse_texture_requested();
            auto tex = cmd.mesh->get_diffuse_texture();
            unsigned int tex_id = (tex && tex->get_state() == ResourceState::LoadedGPU)
                                ? tex->get_texture_id() : 0u;
            hash_value(h, tex_id);
        }
    }
    for (const auto& lcmd : state.lights) {
        const LightComponent* l = lcmd.light;
        if (!l) continue;
        hash_value(h, l->color);
        hash_value(h, l->intensity);
        if (lcmd.actor) {
            hash_value(h, lcmd.actor->get_actor_transform().position);
            hash_value(h, lcmd.actor->get_actor_transform().rotation);
            hash_value(h, lcmd.actor->get_actor_transform().scale);
        }
        // Component type participates too - a spot and a point at the same place
        // are different lights.
        const char* tn = typeid(*l).name();
        hash_bytes(h, tn, std::strlen(tn));
        if (auto* spot = dynamic_cast<const SpotLightComponent*>(l)) {
            hash_value(h, spot->inner_angle);
            hash_value(h, spot->outer_angle);
        }
    }
    hash_value(h, state.sky_mode);
    hash_value(h, state.void_color);
    return h;
}

// Forward vector of an actor, matching the convention the light components use.
Vector3 actor_forward(const Transform& t) {
    Matrix4x4 rot = Matrix4x4::rotationZ(t.rotation.z)
                  * Matrix4x4::rotationX(t.rotation.x)
                  * Matrix4x4::rotationY(t.rotation.y);
    return (rot * Vector3{ 0.0f, 0.0f, -1.0f }).normalized();
}
Vector3 actor_axis(const Transform& t, const Vector3& axis) {
    Matrix4x4 rot = Matrix4x4::rotationZ(t.rotation.z)
                  * Matrix4x4::rotationX(t.rotation.x)
                  * Matrix4x4::rotationY(t.rotation.y);
    return rot * axis;
}

void build_tesla_scene(const RenderState& state, Renderer& renderer) {
    TeslaRenderer& tesla = renderer.tesla;

    // Path-trace in a frame centred on the scene rather than on the world origin.
    // Float32 spacing at 100k units out is ~0.008, which swamps the 1e-4 ray epsilon
    // and produces self-intersection acne; recentring keeps the numbers small
    // regardless of where in the world the level actually sits.
    DVector3 origin{ 0.0, 0.0, 0.0 };
    int origin_samples = 0;
    for (const auto& cmd : state.meshes) {
        if (cmd.is_invisible) continue;
        origin += cmd.transform.position;
        ++origin_samples;
    }
    if (origin_samples > 0) origin = origin / static_cast<double>(origin_samples);
    renderer.tesla_world_origin = origin;

    const Vector3 origin_f = origin.to_vec3();

    tesla.begin_scene();

    // Materials are deduplicated so the emitter table and the GPU upload stay small
    // when a scene reuses one material across many meshes.
    struct MaterialKey { Vector3 color; float metallic, roughness, emissive; int texture; };
    std::vector<MaterialKey> keys;
    auto material_for = [&](const RenderMeshCommand& cmd, int texture) -> int {
        for (size_t i = 0; i < keys.size(); ++i) {
            const MaterialKey& k = keys[i];
            if (k.texture == texture &&
                std::abs(k.color.x - cmd.color.x) < 1e-6f &&
                std::abs(k.color.y - cmd.color.y) < 1e-6f &&
                std::abs(k.color.z - cmd.color.z) < 1e-6f &&
                std::abs(k.metallic - cmd.metallic) < 1e-6f &&
                std::abs(k.roughness - cmd.roughness) < 1e-6f &&
                std::abs(k.emissive - cmd.emissive) < 1e-6f) {
                return static_cast<int>(i);
            }
        }
        TeslaMaterial m;
        m.base_color = cmd.color;
        m.metallic   = cmd.metallic;
        m.roughness  = cmd.roughness;
        m.albedo_texture = texture;
        // Emission is radiance, tinted by the surface colour - the same convention
        // the raster path's emissive term uses.
        m.emission   = cmd.color * cmd.emissive;
        keys.push_back({ cmd.color, cmd.metallic, cmd.roughness, cmd.emissive, texture });
        return tesla.add_material(m);
    };

    int textured_meshes = 0;
    for (const auto& cmd : state.meshes) {
        if (cmd.is_invisible || !cmd.mesh) continue;

        // Imported meshes resolve their diffuse map lazily from render(), which does
        // not run in TESLA mode - so ask for it explicitly.
        cmd.mesh->ensure_diffuse_texture_requested();
        int texture = -1;
        if (auto tex = cmd.mesh->get_diffuse_texture()) {
            if (tex->get_state() == ResourceState::LoadedGPU) {
                texture = tesla.add_texture(tex->get_texture_id());
                if (texture >= 0) ++textured_meshes;
            }
        }

        const int material = material_for(cmd, texture);

        Matrix4x4 model = cmd.transform.get_matrix();
        Matrix4x4 normal_mat = model.inverse().transpose();

        const auto* verts = &cmd.mesh->get_vertices();
        const auto* inds  = &cmd.mesh->get_indices();
        if (cmd.mesh->get_mesh_resource()) {
            verts = &cmd.mesh->get_mesh_resource()->get_cpu_vertices();
            inds  = &cmd.mesh->get_mesh_resource()->get_cpu_indices();
        }
        if (!verts || !inds || inds->size() < 3) continue;

        auto transform_normal = [&](const Vector3& n) {
            const auto& m = normal_mat.m;
            return Vector3{
                m[0]*n.x + m[4]*n.y + m[8]*n.z,
                m[1]*n.x + m[5]*n.y + m[9]*n.z,
                m[2]*n.x + m[6]*n.y + m[10]*n.z,
            }.normalized();
        };

        for (size_t i = 0; i + 2 < inds->size(); i += 3) {
            TeslaTriangle tri;
            tri.material = material;
            tri.v0 = (model * (*verts)[(*inds)[i + 0]].position) - origin_f;
            tri.v1 = (model * (*verts)[(*inds)[i + 1]].position) - origin_f;
            tri.v2 = (model * (*verts)[(*inds)[i + 2]].position) - origin_f;
            tri.n0 = transform_normal((*verts)[(*inds)[i + 0]].normal);
            tri.n1 = transform_normal((*verts)[(*inds)[i + 1]].normal);
            tri.n2 = transform_normal((*verts)[(*inds)[i + 2]].normal);
            tri.uv0 = (*verts)[(*inds)[i + 0]].uv;
            tri.uv1 = (*verts)[(*inds)[i + 1]].uv;
            tri.uv2 = (*verts)[(*inds)[i + 2]].uv;
            tesla.add_triangle(tri);
        }
    }

    TeslaSky sky;
    sky.enabled = true;
    bool sky_configured = false;

    for (const auto& lcmd : state.lights) {
        LightComponent* l = lcmd.light;
        if (!l) continue;

        // A sky light drives the environment rather than becoming an analytic light.
        if (auto* sl = dynamic_cast<SkyLightComponent*>(l)) {
            sky.intensity = sl->intensity;
            sky.zenith  = sl->color * 0.55f;
            sky.horizon = sl->color;
            sky.ground  = sl->color * 0.25f;
            sky_configured = true;
            continue;
        }

        Transform t = lcmd.actor ? lcmd.actor->get_actor_transform() : Transform{};

        TeslaLight light;
        light.color     = l->color;
        light.intensity = l->intensity;
        light.position  = t.position.to_vec3() - origin_f;

        if (auto* dl = dynamic_cast<DirectionalLightComponent*>(l)) {
            light.type = TeslaLightType::Directional;
            light.direction = dl->get_direction();
            // The sun's true angular radius. It costs nothing and it is what gives
            // shadow edges that soften with distance from the occluder.
            light.angular_radius = 0.00465f;
        } else if (auto* sp = dynamic_cast<SpotLightComponent*>(l)) {
            light.type = TeslaLightType::Spot;
            light.direction = sp->get_direction();
            light.cos_inner = std::cos(sp->inner_angle * 3.14159265f / 180.0f);
            light.cos_outer = std::cos(sp->outer_angle * 3.14159265f / 180.0f);
            light.radius = 0.05f;
        } else if (dynamic_cast<AreaLightComponent*>(l)) {
            light.type = TeslaLightType::Area;
            light.direction = actor_forward(t);
            // Half-extents from the actor's scale, so resizing the actor resizes the
            // emitter and the penumbra with it.
            light.u_axis = actor_axis(t, Vector3{ 1.0f, 0.0f, 0.0f }) * (t.scale.x * 0.5f);
            light.v_axis = actor_axis(t, Vector3{ 0.0f, 1.0f, 0.0f }) * (t.scale.y * 0.5f);
        } else {
            light.type = TeslaLightType::Point;
            light.direction = actor_forward(t);
            light.radius = 0.05f;
        }

        tesla.add_light(light);
    }

    if (!sky_configured && state.sky_mode == 2) {
        // Flat void background.
        sky.zenith = sky.horizon = sky.ground = state.void_color;
    }
    tesla.set_sky(sky);

    // The HDRI is what actually lights scenes like the reflections showcase, whose
    // analytic lights are a 0.12-intensity moon. A path tracer has no separate
    // "ambient" channel to fold it into - the environment has to be visible to rays.
    tesla.set_environment(renderer.env_map_cpu.empty() ? nullptr : renderer.env_map_cpu.data(),
                          renderer.env_map_width, renderer.env_map_height,
                          renderer.env_map_texture);

    tesla.end_scene();

    if (tesla.triangle_count() > 0) {
        std::cout << "[TESLA] scene: " << tesla.triangle_count() << " triangles, "
                  << textured_meshes << " textured meshes, backend "
                  << tesla.backend_name() << std::endl;
    }
}

} // namespace

void Engine::render(RenderState& state, float render_delta_time) {
    // Start ImGui Frame
    window->new_frame();

    if (current_state == EngineState::MainMenu) {
        // Headless/automated runs, and the developer "skip menus" option, go straight
        // through to project setup rather than waiting on a click.
        if (auto_launch || active_config.dev_auto_enter_editor) {
            current_state = EngineState::ProjectBrowser;
            // render() has already opened an ImGui frame; returning without closing
            // it leaves the frame dangling and ImGui asserts on the next NewFrame.
            window->render_imgui();
            return;
        }
        glClearColor(0.07f, 0.08f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw_main_menu();
        window->render_imgui();
        if (screenshot_requested) {
            take_screenshot(screenshot_requested_path);
            screenshot_requested = false;
            screenshot_requested_path = "screenshot.bmp";
        }
        return;
    }

    if (current_state == EngineState::ProjectBrowser) {
        // Draw Browser GUI
        bool launch_clicked = browser_ui.render(active_config);
        
        // Blank screen background for browser
        if (runtime_initialized) renderer->unbind_resolve_fbo();
        glViewport(0, 0, window->get_width(), window->get_height());
        glClearColor(0.10f, 0.11f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (launch_clicked || auto_launch || active_config.dev_auto_enter_editor) {
            save_engine_options();   // keep browser-side choices (resolution, vsync, ...)
            auto_launch = false; // one-shot

            // This is the point the heavy subsystems finally come up, so the loading
            // screen belongs here - after the user has picked a project, not before
            // the menus have even been shown.
            //
            // render() opened an ImGui frame at the top and the loading pump opens
            // its own frames per presented frame; close this one first or ImGui
            // asserts on the nested NewFrame. run() swaps buffers after render()
            // returns, which simply re-presents the final loading frame.
            window->render_imgui();
            if (!initialize_runtime()) {
                is_running = false;
                return;
            }

            // Take *input focus*, not merely raise. X11 only delivers mouse events to
            // the focused window, so an unfocused editor receives nothing at all -
            // which is why right-drag camera control appeared dead until you clicked
            // something (any click focuses the window, and then it works).
            //
            // SDL_RaiseWindow alone is not sufficient: it restacks the window but
            // leaves focus to the window manager's policy. SDL_SetWindowInputFocus
            // asks for focus explicitly. It is X11-only and can legitimately fail, so
            // its result is deliberately not treated as an error.
            focus_main_window();
            std::lock_guard<std::mutex> lock(scene_mutex);
            load_project_template();
            current_state = EngineState::Editor;

            // Developer "spawn on start": drop a subject actor in front of the camera
            // and select it, so its properties are already open for editing.
            if (active_config.dev_spawn_on_start > 0) {
                Actor* spawned = nullptr;
                switch (active_config.dev_spawn_on_start) {
                    case 1: spawned = spawn_actor_by_id(0); break;   // Cube
                    case 2: spawned = spawn_actor_by_id(1); break;   // Sphere
                    case 3: spawned = spawn_actor_by_id(4); break;   // Static Light Ray
                    default: break;
                }
                if (spawned) {
                    spawned->get_actor_transform().position = {
                        camera_pos.x, camera_pos.y, camera_pos.z - 5.0
                    };
                    editor_ui.clear_selection();
                    editor_ui.select_actor(spawned);
                }
            }

            load_static_bake();

            // Trigger begin play on the loaded actors
            for (auto& actor : actors) {
                actor->begin_play();
            }

            // The ImGui frame was already closed before loading started, so skip the
            // shared tail below - it would flush a second time on a finished frame.
            // The editor renders normally from the next iteration.
            return;
        }
    } else {
        bool enable_msaa = true;
        bool enable_tesla = editor_ui.is_tesla_mode();

        
        renderer->lights.clear();
        for (auto& lcmd : state.lights) {
            renderer->lights.push_back(lcmd.light);
            // Apply global settings to the directional light component
            if (dynamic_cast<DirectionalLightActor*>(lcmd.actor.get())) {
                lcmd.light->enable_3d_clouds = state.enable_3d_clouds;
                lcmd.light->sky_mode = state.sky_mode;
                lcmd.light->void_color = state.void_color;
                lcmd.light->enable_ray_tracing = state.enable_ray_tracing;
                lcmd.light->enable_embree = state.enable_embree;
            }
        }

        // Update View matrix from camera position & rotation
        Matrix4x4 current_view = state.view_matrix;
        renderer->set_view_matrix(current_view);
        float aspect = (float)renderer->fbo_width / (float)renderer->fbo_height;
        if (aspect < 0.1f) aspect = 0.1f;
        
        if (active_config.is_2d_mode) {
            renderer->set_projection_matrix(Matrix4x4::orthographic(-ortho_zoom * aspect, ortho_zoom * aspect, -ortho_zoom, ortho_zoom, -1000.0f, 1000.0f));
        } else {
            renderer->set_projection_matrix(Matrix4x4::perspective(45.0f, aspect, 0.1f, 1000.0f));
        }
        // Rebuilt after both matrices are final, because the planes are derived
        // from their product.
        renderer->update_frustum_planes();
        renderer->culled_by_frustum = 0;
        renderer->set_camera_position(state.camera_pos.to_vec3());
        // Set the large-world-coordinates origin before the shadow pass, not after:
        // the shadow pass builds its light matrices relative to this origin, so
        // setting it later left shadows using the previous frame's camera position.
        renderer->set_camera_pos(state.camera_pos); // LWC origin
        
        Vector3 forward = { -current_view.m[2], -current_view.m[6], -current_view.m[10] };
        Vector3 up = { current_view.m[1], current_view.m[5], current_view.m[9] };
        // Update Audio Listener
        AudioEngine::get().set_listener(state.camera_pos.to_vec3(), forward, up);

        // Listener velocity, differentiated from the camera. Doppler needs relative
        // motion, and half of that is the listener's - a stationary siren passed at
        // speed shifts exactly as much as a moving one heard from standing still.
        {
            static DVector3 previous_listener_position = state.camera_pos;
            static bool have_previous_listener = false;
            if (have_previous_listener && render_delta_time > 1e-5f) {
                const DVector3 delta = state.camera_pos - previous_listener_position;
                AudioEngine::get().set_listener_velocity(
                    Vector3{ static_cast<float>(delta.x / render_delta_time),
                             static_cast<float>(delta.y / render_delta_time),
                             static_cast<float>(delta.z / render_delta_time) });
            }
            previous_listener_position = state.camera_pos;
            have_previous_listener = true;
        }
        renderer->enable_ue4_lighting = active_config.enable_ue4_lighting;
        renderer->wireframe_mode = editor_ui.is_wireframe_mode();
        renderer->enable_msaa = enable_msaa; // Re-used for FXAA toggle
        renderer->enable_taa = state.enable_taa;
        renderer->upscaling_scale = state.upscaling_scale;
        renderer->enable_ray_tracing = state.enable_ray_tracing;
        renderer->enable_embree = state.enable_embree;
        renderer->enable_tesla = enable_tesla;

        renderer->profiler.begin_frame();

        // 1. Shadow Pass (for simulated ray tracing shadows)
        if (!enable_tesla) {
            ScopedGpuPass _shadow(renderer->profiler, RenderProfiler::Shadow);
            renderer->begin_shadow_pass();
            for (auto& cmd : state.meshes) {
                if (!cmd.is_invisible) {
                    renderer->render_mesh_shadow(*cmd.mesh, cmd.transform, &cmd.bone_matrices,
                                                 cmd.lod_mesh.get());
                }
            }
            for (auto& cmd : state.terrains) {
                if (!cmd.terrain) continue;
                renderer->render_terrain_shadow(*cmd.terrain, cmd.transform);
                if (auto foliage = cmd.terrain->get_foliage_mesh()) {
                    renderer->render_foliage_shadow(*cmd.terrain, *foliage, cmd.transform);
                }
            }
            renderer->end_shadow_pass();
        }

        if (enable_tesla) {
            if (!renderer->is_offline_rendering) {
                renderer->start_offline_render(renderer->fbo_width, renderer->fbo_height);
            }
            // Rebuild on content change, not on mode entry. The old code keyed the
            // rebuild on "are we already rendering", so moving an object or editing a
            // material mid-render left the BVH holding the previous state.
            uint64_t signature = tesla_scene_signature(state);
            if (signature != renderer->tesla_scene_signature) {
                renderer->tesla_scene_signature = signature;
                build_tesla_scene(state, *renderer);
            }
            renderer->step_offline_render();
        } else {
            if (renderer->is_offline_rendering) {
                renderer->cancel_offline_render();
            }
            
            // 2. Main Render Pass (Rasterization)
            renderer->set_camera_pos(state.camera_pos); // LWC
            {
                ScopedGpuPass _geo(renderer->profiler, RenderProfiler::Geometry);
                renderer->bind_fbo();
                renderer->begin_frame();

                for (auto& cmd : state.meshes) {
                    if (cmd.has_bounds) {
                        const Vector3 relative_center =
                            (DVector3{ static_cast<double>(cmd.bounds_center_world.x),
                                       static_cast<double>(cmd.bounds_center_world.y),
                                       static_cast<double>(cmd.bounds_center_world.z) }
                             - state.camera_pos).to_vec3();
                        if (!renderer->is_inside_frustum(relative_center, cmd.bounds_radius_world)) {
                            renderer->culled_by_frustum++;
                            continue;
                        }
                        // Occlusion answers come from the previous frame's queries.
                        // The shadow pass above deliberately does not consult them:
                        // an object hidden from the camera can still cast a shadow
                        // that is not.
                        if (renderer->was_occluded(cmd.mesh)) {
                            renderer->culled_by_occlusion++;
                            continue;
                        }
                    }
                    renderer->set_ambient_cube(cmd.ambient_cube);
                    renderer->render_mesh(*cmd.mesh, cmd.transform, cmd.color, cmd.metallic, cmd.roughness,
                                           cmd.clearcoat, cmd.clearcoat_roughness, cmd.sheen, cmd.subsurface, cmd.emissive,
                                           cmd.is_invisible, cmd.is_selected, &cmd.bone_matrices,
                                           cmd.lod_mesh.get(), cmd.custom_shader.get(),
                                           &cmd.custom_shader_values);
                }

                for (auto& cmd : state.terrains) {
                    if (!cmd.terrain) continue;
                    renderer->set_ambient_cube(cmd.ambient_cube);
                    renderer->render_terrain(*cmd.terrain, cmd.transform);
                    if (auto foliage = cmd.terrain->get_foliage_mesh()) {
                        renderer->render_foliage(*cmd.terrain, *foliage, cmd.transform);
                    }
                }

                // Occlusion queries, against the depth the pass above just wrote and
                // while the G-buffer is still bound. Every object still inside the
                // frustum is tested, including ones this frame skipped, so a hidden
                // object can be found again the moment it is uncovered.
                renderer->begin_occlusion_pass();
                for (auto& cmd : state.meshes) {
                    if (!cmd.has_bounds || cmd.is_invisible) continue;
                    const Vector3 relative_center =
                        (DVector3{ static_cast<double>(cmd.bounds_center_world.x),
                                   static_cast<double>(cmd.bounds_center_world.y),
                                   static_cast<double>(cmd.bounds_center_world.z) }
                         - state.camera_pos).to_vec3();
                    if (!renderer->is_inside_frustum(relative_center, cmd.bounds_radius_world)) continue;
                    renderer->submit_occlusion_test(cmd.mesh,
                                                    cmd.transform.get_relative_matrix(state.camera_pos),
                                                    cmd.bounds_local_min, cmd.bounds_local_max);
                }
                renderer->end_occlusion_pass();

                renderer->end_frame();
            }
            {
                // unbind_fbo runs the deferred lighting resolve, so it is the lighting
                // pass as far as timing is concerned - not a buffer unbind.
                ScopedGpuPass _lit(renderer->profiler, RenderProfiler::Lighting);
                renderer->unbind_fbo(); // Performs Lighting Pass to resolve_fbo
            }
            
            // Forward Pass (Skybox) on resolve_fbo
            renderer->bind_resolve_fbo();
            renderer->render_skybox();
            // Authored light shafts scatter on top of the lit scene and the sky, but
            // before tonemapping, so they bloom and expose like any other emissive.
            if (!state.slr_volumes.empty()) {
                std::vector<SLRVolumeInstance> slr_instances;
                slr_instances.reserve(state.slr_volumes.size());
                for (const auto& cmd : state.slr_volumes) {
                    SLRVolumeInstance inst;
                    inst.inv_model = cmd.transform.get_relative_matrix(state.camera_pos).inverse();
                    // Light travels along the volume's local -Y (down the shaft),
                    // rotated into world space. Composed the same way Transform does.
                    {
                        const Vector3& r = cmd.transform.rotation;
                        Matrix4x4 rot = Matrix4x4::rotationZ(r.z) * Matrix4x4::rotationX(r.x) * Matrix4x4::rotationY(r.y);
                        inst.beam_dir = (rot * Vector3{ 0.0f, -1.0f, 0.0f }).normalized();
                    }
                    inst.color = cmd.color;
                    inst.alpha = cmd.alpha;
                    inst.shape = cmd.shape;
                    inst.sharpness = cmd.sharpness;
                    inst.intensity = cmd.intensity;
                    inst.falloff = cmd.falloff;
                    inst.core = cmd.core;
                    slr_instances.push_back(inst);
                }
                renderer->render_slr_volumes(slr_instances);
            }

            // Particles, after lighting and into the same resolve target. They are
            // translucent and emissive, which the G-buffer cannot represent.
            for (auto& cmd : state.particles) {
                renderer->render_particles(cmd.instances, cmd.blend_mode, cmd.texture_path);
            }
            renderer->unbind_resolve_fbo();
        }

        // 3. FXAA & Tonemapping Pass
        renderer->update_exposure(render_delta_time); // Eye adaptation: reads resolve_texture before FXAA/tonemap consumes it
        // Screen-space post-processing runs only for the rasterised path. All four
        // passes read the G-buffer and the resolve texture, and in TESLA mode the
        // geometry and lighting passes never write either - so this was compositing
        // the previous rasterised frame's AO, bloom and reflections onto a
        // path-traced image, then running FXAA over Monte Carlo noise. TESLA
        // presents itself instead, in step_offline_render().
        if (!enable_tesla) {
            { ScopedGpuPass _p(renderer->profiler, RenderProfiler::SSAO);    renderer->render_ssao(); }     // AO + screen-space bounce light, consumed by resolve_fxaa
            { ScopedGpuPass _p(renderer->profiler, RenderProfiler::Bloom);   renderer->render_bloom(); }    // bloom mip pyramid, consumed by resolve_fxaa
            { ScopedGpuPass _p(renderer->profiler, RenderProfiler::GodRays); renderer->render_god_rays(); }
            { ScopedGpuPass _p(renderer->profiler, RenderProfiler::Resolve); renderer->resolve_fxaa(); }
        }


        // Pass the final texture (FXAA resolve) to ImGui
        unsigned int viewport_texture = renderer->get_viewport_texture();

#ifdef STANDALONE_GAME
        // Draw the viewport texture full screen using ImGui without any editor panels
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2((float)window->get_width(), (float)window->get_height()));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("Game", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground);
        ImGui::Image((void*)(intptr_t)viewport_texture, ImVec2((float)window->get_width(), (float)window->get_height()), ImVec2(0, 1), ImVec2(1, 0));
        ImGui::End();
        ImGui::PopStyleVar(2);
#else
        bool viewport_clicked = false;
        float ndc_x = 0.0f;
        float ndc_y = 0.0f;

        // Render full Unreal-style Editor layout panels
        EditorRequest req;
        {
            std::lock_guard<std::mutex> lock(scene_mutex); // Lock when drawing UI
            // Import anything dropped onto the window since the last frame.
            for (const std::string& dropped : pending_dropped_files) {
                if (Editor::is_model_file(dropped)) {
                    editor_ui.spawn_model_actor(actors, dropped);
                } else {
                    std::cout << "[Import] Unsupported drop ignored: " << dropped << std::endl;
                }
            }
            pending_dropped_files.clear();

            req = editor_ui.render(actors, viewport_texture, editor_logo_texture, screenshot_requested, viewport_clicked, ndc_x, ndc_y, get_view_matrix(), renderer->get_projection_matrix(), renderer.get(), current_state);
        }
        
        if (req == EditorRequest::Play) {
            std::lock_guard<std::mutex> lock(scene_mutex);
            capture_pie_snapshot();
            current_state = EngineState::PlayInEditor;
            editor_ui.clear_selection();
            for (auto& actor : actors) {
                actor->begin_play();
            }
        } else if (req == EditorRequest::Pause) {
            std::lock_guard<std::mutex> lock(scene_mutex);
            current_state = EngineState::PlayInEditorPaused;
        } else if (req == EditorRequest::Resume) {
            std::lock_guard<std::mutex> lock(scene_mutex);
            current_state = EngineState::PlayInEditor;
        } else if (req == EditorRequest::Stop) {
            std::lock_guard<std::mutex> lock(scene_mutex);
            current_state = EngineState::Editor;
            editor_ui.clear_selection();
            restore_pie_snapshot();
        }

        // A click that landed on a game UI widget must not also select whatever
        // scene object happens to be behind it. The canvases are submitted later in
        // this same frame, so what is available here is last frame's answer - which
        // is correct unless the layout moved between the two, and a UI that moves
        // out from under the cursor mid-click has bigger problems.
        if (viewport_clicked && !ui_consumed_mouse) {
            Actor* best_actor = nullptr;
            float closest_t = 99999.0f;

            // Generate Ray Direction
            float aspect = (float)renderer->fbo_width / (float)renderer->fbo_height;
            float fovRad = 45.0f * (3.14159265f / 180.0f);
            float tanHalfFov = std::tan(fovRad / 2.0f);
            
            // View space ray
            Vector3 ray_view = { ndc_x * aspect * tanHalfFov, ndc_y * tanHalfFov, -1.0f };
            
            // Convert to world space
            Matrix4x4 view = get_view_matrix();
            Vector3 right = { view.m[0], view.m[4], view.m[8] };
            Vector3 up = { view.m[1], view.m[5], view.m[9] };
            Vector3 forward = { -view.m[2], -view.m[6], -view.m[10] };
            
            Vector3 ray_dir = (right * ray_view.x) + (up * ray_view.y) + (forward * 1.0f);
            ray_dir = ray_dir.normalized();
            DVector3 ray_dir_d = {static_cast<double>(ray_dir.x), static_cast<double>(ray_dir.y), static_cast<double>(ray_dir.z)};
            DVector3 ray_origin_d = camera_pos;

            for (auto& actor : actors) {
                DVector3 pos = actor->get_actor_transform().position;
                Vector3 scale = actor->get_actor_transform().scale;
                
                // AABB intersection
                DVector3 min_bound = pos - DVector3{scale.x * 0.5, scale.y * 0.5, scale.z * 0.5};
                DVector3 max_bound = pos + DVector3{scale.x * 0.5, scale.y * 0.5, scale.z * 0.5};
                
                double tmin = (min_bound.x - ray_origin_d.x) / ray_dir_d.x;
                double tmax = (max_bound.x - ray_origin_d.x) / ray_dir_d.x;
                if (tmin > tmax) std::swap(tmin, tmax);
                
                double tymin = (min_bound.y - ray_origin_d.y) / ray_dir_d.y;
                double tymax = (max_bound.y - ray_origin_d.y) / ray_dir_d.y;
                if (tymin > tymax) std::swap(tymin, tymax);
                
                if ((tmin <= tymax) && (tymin <= tmax)) {
                    if (tymin > tmin) tmin = tymin;
                    if (tymax < tmax) tmax = tymax;
                    
                    double tzmin = (min_bound.z - ray_origin_d.z) / ray_dir_d.z;
                    double tzmax = (max_bound.z - ray_origin_d.z) / ray_dir_d.z;
                    if (tzmin > tzmax) std::swap(tzmin, tzmax);
                    
                    if ((tmin <= tzmax) && (tzmin <= tmax)) {
                        if (tzmin > tmin) tmin = tzmin;
                        if (tzmax < tmax) tmax = tzmax;
                        
                        // If we hit, tmin is the entry point
                        if (tmin > 0.0 && tmin < closest_t) {
                            closest_t = tmin;
                            best_actor = actor.get();
                        }
                    }
                }
            }
            
            if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) {
                editor_ui.clear_selection();
            }
            if (best_actor) {
                if ((ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift) && editor_ui.is_actor_selected(best_actor)) {
                    // Ctrl/Shift+Click to deselect already selected
                    // we would need to remove it, but for simplicity we can just clear it and not re-add, or let editor handle it.
                    // editor.cpp Selectable already handles this for Outliner, so let's do it manually here:
                    auto& selected = const_cast<std::vector<Actor*>&>(editor_ui.get_selected_actors());
                    for (auto it = selected.begin(); it != selected.end(); ++it) {
                        if (*it == best_actor) {
                            selected.erase(it);
                            break;
                        }
                    }
                } else if (!editor_ui.is_actor_selected(best_actor)) {
                    editor_ui.select_actor(best_actor);
                }
            }
        }
#endif
    }

    // Must be submitted before render_imgui(), which flushes the frame's draw data.
    draw_unsaved_changes_prompt();

    // Render ImGui draw pipeline
    if (runtime_initialized) renderer->unbind_resolve_fbo();
    glViewport(0, 0, window->get_width(), window->get_height());
    glClearColor(0.1f, 0.11f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT); // Not clearing depth since ImGui doesn't need it, but good practice
    draw_script_hud();
    draw_ui_canvases();
    window->render_imgui();

    if (screenshot_requested) {
        take_screenshot(screenshot_requested_path);
        screenshot_requested = false;
        screenshot_requested_path = "screenshot.bmp";
    }
}

// Lays out and draws every game UI canvas in the scene.
//
// Runs on the main thread, after the frame has been composited and before ImGui's
// draw data is flushed, because both drawing and reading the pointer have to happen
// inside the same UI frame. The canvases are laid out inside the viewport rectangle
// rather than the window, so a HUD authored against the game view lines up with it
// in the editor as well as in a standalone build.
void Engine::draw_ui_canvases() {
    ui_consumed_mouse = false;
    if (!runtime_initialized) return;

    const bool playing = (current_state == EngineState::PlayInEditor);
    const bool paused  = (current_state == EngineState::PlayInEditorPaused);
    if (current_state != EngineState::Editor && !playing && !paused) return;

    UIRect screen_rect;
#ifdef STANDALONE_GAME
    screen_rect.x = 0.0f;
    screen_rect.y = 0.0f;
    screen_rect.width  = static_cast<float>(window->get_width());
    screen_rect.height = static_cast<float>(window->get_height());
#else
    screen_rect.x = editor_ui.viewport_screen_x;
    screen_rect.y = editor_ui.viewport_screen_y;
    screen_rect.width  = editor_ui.viewport_screen_w;
    screen_rect.height = editor_ui.viewport_screen_h;
    // The viewport panel has not been drawn yet on the very first frame.
    if (screen_rect.width < 1.0f || screen_rect.height < 1.0f) return;
#endif

    ImGuiIO& io = ImGui::GetIO();
    UIInputState input;
    input.mouse_x = io.MousePos.x;
    input.mouse_y = io.MousePos.y;
    input.mouse_down     = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    input.mouse_pressed  = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    input.mouse_released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    // Only a running game takes UI input. In the editor the same mouse is placing
    // actors and dragging gizmos, and a canvas that reacted to it would fight the
    // person authoring it.
    input.interactive = playing;

    // Collected first so they can be drawn in sort order across actors: a pause menu
    // on one actor has to cover a HUD on another, and actor order is not that order.
    struct CanvasEntry {
        Actor* actor = nullptr;
        UICanvasComponent* canvas = nullptr;
    };
    std::vector<CanvasEntry> entries;
    {
        std::lock_guard<std::mutex> lock(scene_mutex);
        for (auto& actor : actors) {
            if (!actor) continue;
            for (auto& comp : actor->get_components()) {
                if (auto* canvas = dynamic_cast<UICanvasComponent*>(comp.get())) {
                    if (!canvas->visible) continue;
                    if (!playing && !paused && !canvas->show_in_editor) continue;
                    entries.push_back({ actor.get(), canvas });
                }
            }
        }
    }
    if (entries.empty()) return;

    std::stable_sort(entries.begin(), entries.end(),
                     [](const CanvasEntry& a, const CanvasEntry& b) {
                         return a.canvas->sort_order < b.canvas->sort_order;
                     });

    // Highest sort order first for input, so the topmost canvas gets the pointer and
    // the ones below it see an already-consumed frame. Drawing then runs in the
    // opposite order inside render(), which is why input has to be resolved here
    // rather than left to the draw loop.
    bool pointer_taken = false;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        UIInputState canvas_input = input;
        if (pointer_taken) {
            canvas_input.mouse_down = false;
            canvas_input.mouse_pressed = false;
            canvas_input.mouse_released = false;
            // Park the pointer far outside the canvas so nothing reports a hover.
            canvas_input.mouse_x = -1.0e6f;
            canvas_input.mouse_y = -1.0e6f;
        }
        it->canvas->render(screen_rect, canvas_input);
        if (it->canvas->consumed_mouse()) {
            pointer_taken = true;
            ui_consumed_mouse = true;
        }
    }

    // Widget events become script calls. Dispatched after every canvas has been
    // laid out, so a handler that hides a panel or rebuilds the tree cannot
    // invalidate a canvas that has not been visited yet.
    if (!playing) return;
    std::lock_guard<std::mutex> lock(scene_mutex);
    for (const CanvasEntry& entry : entries) {
        for (const std::string& widget_name : entry.canvas->get_changed_widgets()) {
            const UIWidget* widget = entry.canvas->find(widget_name);
            entry.actor->dispatch_ui_value_changed(widget_name, widget ? widget->value : 0.0f);
        }
        for (const std::string& widget_name : entry.canvas->get_clicked_widgets()) {
            entry.actor->dispatch_ui_click(widget_name);
        }
        entry.canvas->clear_events();
    }
}

// Renders whatever the running script asked for on top of the finished frame.
// Drawn through the foreground draw list so it sits over the viewport in the editor
// and over the fullscreen game image in a standalone build, without either needing
// to know it exists.
void Engine::draw_script_hud() {
    if (current_state != EngineState::PlayInEditor) return;

    const ScriptHud& hud = script_hud;
    if (hud.pip_count <= 0 && hud.vignette <= 0.001f) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float w = static_cast<float>(window->get_width());
    const float h = static_cast<float>(window->get_height());

    // Edge tint. Four gradient bands rather than a radial falloff: a draw list has
    // no shader, and four quads read as a vignette perfectly well.
    if (hud.vignette > 0.001f) {
        float a = hud.vignette;
        ImU32 solid = IM_COL32(static_cast<int>(hud.vignette_color.x * 255.0f),
                               static_cast<int>(hud.vignette_color.y * 255.0f),
                               static_cast<int>(hud.vignette_color.z * 255.0f),
                               static_cast<int>(a * 190.0f));
        ImU32 clear = IM_COL32(static_cast<int>(hud.vignette_color.x * 255.0f),
                               static_cast<int>(hud.vignette_color.y * 255.0f),
                               static_cast<int>(hud.vignette_color.z * 255.0f), 0);
        float band = h * (0.18f + 0.22f * a);
        float side = w * (0.14f + 0.18f * a);
        dl->AddRectFilledMultiColor(ImVec2(0, 0),        ImVec2(w, band),      solid, solid, clear, clear);
        dl->AddRectFilledMultiColor(ImVec2(0, h - band), ImVec2(w, h),         clear, clear, solid, solid);
        dl->AddRectFilledMultiColor(ImVec2(0, 0),        ImVec2(side, h),      solid, clear, clear, solid);
        dl->AddRectFilledMultiColor(ImVec2(w - side, 0), ImVec2(w, h),         clear, solid, solid, clear);
    }

    // Objective markers, centred along the top.
    if (hud.pip_count > 0) {
        const float r = 7.0f;
        const float gap = 26.0f;
        float total = (hud.pip_count - 1) * gap;
        float cx = w * 0.5f - total * 0.5f;
        float cy = 34.0f;
        for (int i = 0; i < hud.pip_count; ++i) {
            ImVec2 c(cx + i * gap, cy);
            bool on = hud.pip[i] > 0.5f;
            dl->AddCircleFilled(c, r, on ? IM_COL32(150, 225, 255, 235)
                                         : IM_COL32(255, 255, 255, 28), 20);
            dl->AddCircle(c, r + 1.5f, on ? IM_COL32(200, 240, 255, 200)
                                          : IM_COL32(255, 255, 255, 70), 20, 1.6f);
        }
    }
}

// Options and developer settings live alongside the graphics API in
// engine_config.json, so a configured workflow survives a restart.
static const char* kEngineConfigPath = "engine_config.json";

void Engine::load_engine_options() {
    if (!std::filesystem::exists(kEngineConfigPath)) return;
    try {
        nlohmann::json j;
        std::ifstream f(kEngineConfigPath);
        f >> j;
        auto& c = active_config;
        if (j.contains("resolution_width"))  c.resolution_width  = j["resolution_width"].get<int>();
        if (j.contains("resolution_height")) c.resolution_height = j["resolution_height"].get<int>();
        if (j.contains("fullscreen"))        c.fullscreen        = j["fullscreen"].get<bool>();
        if (j.contains("vsync"))             c.vsync             = j["vsync"].get<bool>();
        if (j.contains("master_volume"))     c.master_volume     = j["master_volume"].get<float>();
        if (j.contains("enable_ssr"))        c.enable_ssr        = j["enable_ssr"].get<bool>();
        if (j.contains("enable_bloom"))      c.enable_bloom      = j["enable_bloom"].get<bool>();
        if (j.contains("enable_taa"))        c.enable_taa_option = j["enable_taa"].get<bool>();
        if (j.contains("field_of_view"))     c.field_of_view     = j["field_of_view"].get<float>();
        if (j.contains("sky_hdri"))         c.sky_hdri          = j["sky_hdri"].get<std::string>();
        if (j.contains("dev_auto_enter_editor")) c.dev_auto_enter_editor = j["dev_auto_enter_editor"].get<bool>();
        if (j.contains("dev_camera_x"))     c.dev_camera_x     = j["dev_camera_x"].get<float>();
        if (j.contains("dev_camera_y"))     c.dev_camera_y     = j["dev_camera_y"].get<float>();
        if (j.contains("dev_camera_z"))     c.dev_camera_z     = j["dev_camera_z"].get<float>();
        if (j.contains("dev_camera_yaw"))   c.dev_camera_yaw   = j["dev_camera_yaw"].get<float>();
        if (j.contains("dev_camera_pitch")) c.dev_camera_pitch = j["dev_camera_pitch"].get<float>();
        if (j.contains("dev_spawn_on_start")) c.dev_spawn_on_start = j["dev_spawn_on_start"].get<int>();
        // Remembering the template makes "skip menus" actually land in the scene you
        // were last working on, rather than always the blank one.
        if (j.contains("selected_template")) {
            int t = j["selected_template"].get<int>();
            if (t >= 0 && t <= 4) c.selected_template = static_cast<EngineTemplate>(t);
        }
        if (j.contains("dev_show_startup_timings")) c.dev_show_startup_timings = j["dev_show_startup_timings"].get<bool>();

        if (j.contains("collision_layers")) {
            PhysicsEngine::reset_layers();
            int layer = 0;
            for (const auto& entry : j["collision_layers"]) {
                if (layer >= PhysicsEngine::kLayerCount) break;
                PhysicsEngine::set_layer_name(layer, entry.value("name", std::string()));
                // The mask is written whole rather than through set_layers_collide,
                // which is symmetric and would fight a half-built matrix as it is
                // being restored row by row.
                const uint32_t mask = entry.value("mask", 0xFFFFFFFFu);
                for (int other = 0; other < PhysicsEngine::kLayerCount; ++other) {
                    if ((mask & (1u << other)) == 0) {
                        PhysicsEngine::set_layers_collide(layer, other, false);
                    }
                }
                ++layer;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[Config] Failed to read options: " << e.what() << std::endl;
    }
}

void Engine::save_engine_options() {
    try {
        nlohmann::json j;
        if (std::filesystem::exists(kEngineConfigPath)) {
            std::ifstream f(kEngineConfigPath);
            f >> j;   // preserve keys written elsewhere (e.g. graphics_api)
        }
        const auto& c = active_config;
        j["resolution_width"]  = c.resolution_width;
        j["resolution_height"] = c.resolution_height;
        j["fullscreen"]        = c.fullscreen;
        j["vsync"]             = c.vsync;
        j["master_volume"]     = c.master_volume;
        j["enable_ssr"]        = c.enable_ssr;
        j["enable_bloom"]      = c.enable_bloom;
        j["enable_taa"]        = c.enable_taa_option;
        j["field_of_view"]     = c.field_of_view;
        j["sky_hdri"]          = c.sky_hdri;
        j["dev_auto_enter_editor"] = c.dev_auto_enter_editor;
        j["dev_camera_x"]     = c.dev_camera_x;
        j["dev_camera_y"]     = c.dev_camera_y;
        j["dev_camera_z"]     = c.dev_camera_z;
        j["dev_camera_yaw"]   = c.dev_camera_yaw;
        j["dev_camera_pitch"] = c.dev_camera_pitch;
        j["dev_spawn_on_start"] = c.dev_spawn_on_start;
        j["selected_template"] = static_cast<int>(c.selected_template);
        j["dev_show_startup_timings"] = c.dev_show_startup_timings;

        // Collision layers are project-wide, not per scene: two scenes in the same
        // game must agree on what "layer 3" means or a prefab moved between them
        // starts colliding with different things.
        {
            nlohmann::json layers = nlohmann::json::array();
            for (int layer = 0; layer < PhysicsEngine::kLayerCount; ++layer) {
                layers.push_back({ {"name", PhysicsEngine::get_layer_name(layer)},
                                   {"mask", PhysicsEngine::get_layer_mask(layer)} });
            }
            j["collision_layers"] = layers;
        }

        std::ofstream out(kEngineConfigPath);
        out << j.dump(4);
    } catch (const std::exception& e) {
        std::cerr << "[Config] Failed to write options: " << e.what() << std::endl;
    }
}

// Pushes option values into the systems that actually consume them.
// Offline static light bake.
//
// Scope, stated plainly: this bakes one irradiance value per static actor, not a
// lightmap. A true lightmap pipeline needs per-mesh UV atlas generation (xatlas or
// equivalent), texel-space rasterisation and a texture cache; that is a subsystem in
// its own right, not something to half-build. What this does do is real: for every
// actor marked static it integrates incoming light from the scene's lights over the
// hemisphere, attenuates by a coarse visibility test against other static geometry,
// and caches the result so it costs nothing at runtime.
void Engine::bake_static_lighting() {
    std::lock_guard<std::mutex> lock(scene_mutex);

    // Gather occluder bounds once. Sphere bounds are coarse, but they are enough to
    // capture "this object is tucked behind that one" which is the bulk of what a
    // bake contributes visually.
    struct Occluder { DVector3 center; double radius; };
    std::vector<Occluder> occluders;
    for (auto& a : actors) {
        if (!a->is_static) continue;
        const Transform& t = a->get_actor_transform();
        double r = 0.5 * std::max(std::max(std::fabs(t.scale.x), std::fabs(t.scale.y)), std::fabs(t.scale.z));
        occluders.push_back({ t.position, r });
    }

    struct BakeLight { DVector3 pos; Vector3 color; float intensity; bool directional; DVector3 dir; };
    std::vector<BakeLight> lights;
    for (auto& a : actors) {
        for (auto& comp : a->get_components()) {
            auto* lc = dynamic_cast<LightComponent*>(comp.get());
            if (!lc) continue;
            BakeLight bl;
            bl.pos = a->get_actor_transform().position;
            bl.color = lc->color;
            bl.intensity = lc->intensity;
            bl.directional = (dynamic_cast<DirectionalLightComponent*>(lc) != nullptr);
            if (bl.directional) {
                Vector3 d = static_cast<DirectionalLightComponent*>(lc)->get_direction();
                bl.dir = { d.x, d.y, d.z };
            }
            lights.push_back(bl);
        }
    }

    int baked = 0;
    for (auto& a : actors) {
        if (!a->is_static) continue;
        const DVector3 p = a->get_actor_transform().position;
        float irradiance = 0.0f;

        for (const BakeLight& bl : lights) {
            float contribution = 0.0f;
            DVector3 to_light;
            if (bl.directional) {
                to_light = { -bl.dir.x, -bl.dir.y, -bl.dir.z };
                contribution = bl.intensity;
            } else {
                to_light = { bl.pos.x - p.x, bl.pos.y - p.y, bl.pos.z - p.z };
                double d2 = to_light.x * to_light.x + to_light.y * to_light.y + to_light.z * to_light.z;
                if (d2 < 0.0001) continue;
                contribution = bl.intensity / static_cast<float>(d2);
            }

            // Visibility: march toward the light and drop the contribution if another
            // static occluder sits in the way.
            double len = std::sqrt(to_light.x * to_light.x + to_light.y * to_light.y + to_light.z * to_light.z);
            if (len > 0.0001) {
                DVector3 step = { to_light.x / len, to_light.y / len, to_light.z / len };
                double travel = bl.directional ? 30.0 : len;
                bool blocked = false;
                for (int i = 1; i <= 12 && !blocked; ++i) {
                    double t = travel * (static_cast<double>(i) / 12.0);
                    DVector3 sp = { p.x + step.x * t, p.y + step.y * t, p.z + step.z * t };
                    for (const Occluder& o : occluders) {
                        double dx = sp.x - o.center.x, dy = sp.y - o.center.y, dz = sp.z - o.center.z;
                        double dist2 = dx * dx + dy * dy + dz * dz;
                        // Skip the actor's own bounds.
                        double self_dx = o.center.x - p.x, self_dy = o.center.y - p.y, self_dz = o.center.z - p.z;
                        if (self_dx * self_dx + self_dy * self_dy + self_dz * self_dz < 0.0001) continue;
                        if (dist2 < o.radius * o.radius) { blocked = true; break; }
                    }
                }
                if (blocked) contribution *= 0.25f;   // shadowed, not pitch black
            }

            float luma = 0.2126f * bl.color.x + 0.7152f * bl.color.y + 0.0722f * bl.color.z;
            irradiance += contribution * luma;
        }

        // Only the indirect share is baked; direct light is still evaluated live, so
        // adding the full value here would double-count it.
        a->baked_irradiance = std::min(irradiance * 0.06f, 2.0f);
        a->has_baked_lighting = true;
        ++baked;
    }

    // Cache alongside the scene so a later run can skip the pass entirely.
    try {
        std::filesystem::create_directories("Content/Bakes");
        nlohmann::json j;
        j["version"] = 1;
        for (auto& a : actors) {
            if (!a->is_static) continue;
            j["actors"][a->get_name()] = a->baked_irradiance;
        }
        std::ofstream out("Content/Bakes/scene_bake.json");
        out << j.dump(2);
    } catch (const std::exception& e) {
        std::cerr << "[Bake] Failed to write bake cache: " << e.what() << std::endl;
    }

    std::cout << "[Bake] Static lighting baked for " << baked << " actor(s) -> Content/Bakes/scene_bake.json" << std::endl;
}

void Engine::load_static_bake() {
    std::filesystem::path cache("Content/Bakes/scene_bake.json");
    if (!std::filesystem::exists(cache)) return;
    try {
        nlohmann::json j;
        std::ifstream f(cache);
        f >> j;
        if (!j.contains("actors")) return;
        int restored = 0;
        for (auto& a : actors) {
            auto it = j["actors"].find(a->get_name());
            if (it != j["actors"].end()) {
                a->baked_irradiance = it->get<float>();
                a->has_baked_lighting = true;
                ++restored;
            }
        }
        if (restored > 0) {
            std::cout << "[Bake] Restored baked lighting for " << restored << " actor(s)." << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Bake] Failed to read bake cache: " << e.what() << std::endl;
    }
}

void Engine::set_sky_hdri(const std::string& path) {
    active_config.sky_hdri = path;
    // runtime_initialized, not a null check: the Renderer object exists from the
    // Engine constructor, but has no GL resources until initialize_runtime() runs.
    if (runtime_initialized && !path.empty() && std::filesystem::exists(path)) {
        renderer->load_environment_map(path);
    }
    save_engine_options();
}

void Engine::apply_engine_options() {
    SDL_GL_SetSwapInterval(active_config.vsync ? 1 : 0);
    // Loading an environment map is real GPU work, so it has to wait for the renderer
    // to actually be initialised - not merely constructed. initialize_runtime() calls
    // this again once it is, which is what applies a saved sky.
    if (runtime_initialized && !active_config.sky_hdri.empty() && std::filesystem::exists(active_config.sky_hdri)) {
        renderer->load_environment_map(active_config.sky_hdri);
    }
    if (renderer) renderer->enable_ssr = active_config.enable_ssr;
    // The main menu's Options tab calls this before the heavy subsystems exist, so
    // the miniaudio engine may not be initialised yet; it picks the volume up in
    // initialize_runtime() instead.
    if (AudioEngine::get().initialized()) {
        ma_engine_set_volume(AudioEngine::get().get_engine(), active_config.master_volume);
    }
    SDL_SetWindowFullscreen(window->get_sdl_window(),
                            active_config.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

// Landing screen shown once loading finishes. The engine used to drop straight into the
// project-creation form, which gives no way to simply open existing work or quit.
void Engine::draw_main_menu() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("MainMenu", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float panel_w = 520.0f;
    const float button_h = 42.0f;
    float x = (io.DisplaySize.x - panel_w) * 0.5f;

    if (editor_logo_texture != 0) {
        float logo = 84.0f;
        ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - logo) * 0.5f, 46.0f));
        ImGui::Image((void*)(intptr_t)editor_logo_texture, ImVec2(logo, logo));
    }

    ImGui::SetCursorPos(ImVec2(x, 142.0f));
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "LITHIUM ENGINE");
    ImGui::SetWindowFontScale(1.0f);

    ImGui::SetCursorPos(ImVec2(x, 176.0f));
    ImGui::BeginChild("MainMenuPanel", ImVec2(panel_w, io.DisplaySize.y - 220.0f), false);

    if (ImGui::BeginTabBar("MainMenuTabs")) {
        if (ImGui::BeginTabItem("Project")) {
            main_menu_tab = 0;
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            if (ImGui::Button("New Project", ImVec2(-FLT_MIN, button_h))) {
                current_state = EngineState::ProjectBrowser;
            }
            if (ImGui::Button("Open Project...", ImVec2(-FLT_MIN, button_h))) {
                auto f = pfd::open_file("Open Scene", ".", { "Lithium Scene Files", "*.lithium" });
                if (!f.result().empty()) {
                    std::lock_guard<std::mutex> lock(scene_mutex);
                    std::string path = f.result()[0];
                    if (SceneSerializer::load_scene(path, actors)) {
                        editor_ui.current_scene_path = path;
                        editor_ui.clear_selection();
                        current_state = EngineState::Editor;
                        SDL_RaiseWindow(window->get_sdl_window());
                        for (auto& actor : actors) actor->begin_play();
                    }
                }
            }
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            if (ImGui::Button("Quit", ImVec2(-FLT_MIN, button_h))) {
                request_shutdown();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Options")) {
            main_menu_tab = 1;
            auto& c = active_config;
            bool dirty = false;

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Display");
            ImGui::Separator();

            static const char* res_labels[] = { "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440" };
            static const int res_w[] = { 1280, 1600, 1920, 2560 };
            static const int res_h[] = { 720, 900, 1080, 1440 };
            int res_idx = 0;
            for (int i = 0; i < 4; ++i) {
                if (res_w[i] == c.resolution_width && res_h[i] == c.resolution_height) res_idx = i;
            }
            if (ImGui::Combo("Resolution", &res_idx, res_labels, 4)) {
                c.resolution_width = res_w[res_idx];
                c.resolution_height = res_h[res_idx];
                SDL_SetWindowSize(window->get_sdl_window(), c.resolution_width, c.resolution_height);
                dirty = true;
            }
            if (ImGui::Checkbox("Fullscreen", &c.fullscreen)) dirty = true;
            if (ImGui::Checkbox("VSync", &c.vsync)) dirty = true;
            if (ImGui::SliderFloat("Field of View", &c.field_of_view, 30.0f, 110.0f, "%.0f deg")) dirty = true;

            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Rendering");
            ImGui::Separator();
            if (ImGui::Checkbox("Screen Space Reflections (SSR)", &c.enable_ssr)) dirty = true;
            if (ImGui::Checkbox("Bloom", &c.enable_bloom)) dirty = true;
            if (ImGui::Checkbox("Temporal Anti-Aliasing (TAA)", &c.enable_taa_option)) dirty = true;
            if (ImGui::Checkbox("Ray Tracing / Path Tracing Simulation", &c.enable_raytracing)) dirty = true;
            if (ImGui::Checkbox("UE4 High-Fidelity Lighting", &c.enable_ue4_lighting)) dirty = true;

            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Audio");
            ImGui::Separator();
            if (ImGui::SliderFloat("Master Volume", &c.master_volume, 0.0f, 1.0f, "%.2f")) dirty = true;

            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "System");
            ImGui::Separator();
            ImGui::SliderInt("Physics Tick Rate (Hz)", &c.physics_hz, 30, 240);
            ImGui::TextDisabled("Renderer: see the startup log for GL driver details.");

            if (dirty) {
                apply_engine_options();
                save_engine_options();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Developer")) {
            main_menu_tab = 2;
            auto& c = active_config;
            bool dirty = false;

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::TextWrapped("Shortcuts for iterating quickly: skip straight into a scene "
                               "with the camera already framed on whatever you are working on, "
                               "so a change can be checked in a single launch.");
            ImGui::Dummy(ImVec2(0.0f, 8.0f));

            ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Startup");
            ImGui::Separator();
            if (ImGui::Checkbox("Skip menus, go straight to the editor", &c.dev_auto_enter_editor)) dirty = true;
            if (ImGui::Checkbox("Print startup timings to the log", &c.dev_show_startup_timings)) dirty = true;

            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Camera Start");
            ImGui::Separator();
            float pos[3] = { c.dev_camera_x, c.dev_camera_y, c.dev_camera_z };
            if (ImGui::DragFloat3("Position", pos, 0.1f)) {
                c.dev_camera_x = pos[0]; c.dev_camera_y = pos[1]; c.dev_camera_z = pos[2];
                dirty = true;
            }
            if (ImGui::SliderAngle("Yaw", &c.dev_camera_yaw, -180.0f, 180.0f)) dirty = true;
            if (ImGui::SliderAngle("Pitch", &c.dev_camera_pitch, -80.0f, 80.0f)) dirty = true;
            if (ImGui::Button("Use Current Camera", ImVec2(-FLT_MIN, 0.0f))) {
                c.dev_camera_x = static_cast<float>(camera_pos.x);
                c.dev_camera_y = static_cast<float>(camera_pos.y);
                c.dev_camera_z = static_cast<float>(camera_pos.z);
                c.dev_camera_yaw = camera_rot.y;
                c.dev_camera_pitch = camera_rot.x;
                dirty = true;
            }

            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Spawn On Start");
            ImGui::Separator();
            static const char* spawn_labels[] = { "Nothing", "Cube", "Sphere", "Static Light Ray (SLR)" };
            if (ImGui::Combo("Actor", &c.dev_spawn_on_start, spawn_labels, 4)) dirty = true;
            ImGui::TextDisabled("Spawned in front of the camera and selected, so its\nproperties are open in the Details panel immediately.");

            if (dirty) save_engine_options();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::EndChild();
    ImGui::End();
}

// Confirmation shown when closing the window with unsaved scene changes.
void Engine::draw_unsaved_changes_prompt() {
    if (!pending_quit_prompt) return;

    ImGui::OpenPopup("Unsaved Changes");
    ImVec2 center = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(center.x * 0.5f, center.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::Text("Your project has unsaved changes.");
        ImGui::TextDisabled("Save before closing?");
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        if (ImGui::Button("Save", ImVec2(110, 0))) {
            std::lock_guard<std::mutex> lock(scene_mutex);
            // A cancelled Save dialog must abort the close too, not quietly discard.
            if (editor_ui.save_scene(actors)) {
                pending_quit_prompt = false;
                ImGui::CloseCurrentPopup();
                request_shutdown();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(110, 0))) {
            pending_quit_prompt = false;
            ImGui::CloseCurrentPopup();
            request_shutdown();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) {
            pending_quit_prompt = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Engine::take_screenshot(const std::string& filepath) {
    int w = window->get_width();
    int h = window->get_height();

    std::vector<unsigned char> pixels(w * h * 3);
    // Read pixels from the back buffer
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

    FILE* f = fopen(filepath.c_str(), "wb");
    if (!f) {
        std::cerr << "Failed to open file for screenshot: " << filepath << std::endl;
        return;
    }

    unsigned char bmpFileHeader[14] = { 'B','M', 0,0,0,0, 0,0, 0,0, 54,0,0,0 };
    unsigned char bmpInfoHeader[40] = { 40,0,0,0, 0,0,0,0, 0,0,0,0, 1,0, 24,0 };

    int fileSize = 54 + w * h * 3;
    bmpFileHeader[2] = (unsigned char)(fileSize);
    bmpFileHeader[3] = (unsigned char)(fileSize >> 8);
    bmpFileHeader[4] = (unsigned char)(fileSize >> 16);
    bmpFileHeader[5] = (unsigned char)(fileSize >> 24);

    bmpInfoHeader[4] = (unsigned char)(w);
    bmpInfoHeader[5] = (unsigned char)(w >> 8);
    bmpInfoHeader[6] = (unsigned char)(w >> 16);
    bmpInfoHeader[7] = (unsigned char)(w >> 24);

    bmpInfoHeader[8] = (unsigned char)(h);
    bmpInfoHeader[9] = (unsigned char)(h >> 8);
    bmpInfoHeader[10] = (unsigned char)(h >> 16);
    bmpInfoHeader[11] = (unsigned char)(h >> 24);

    fwrite(bmpFileHeader, 1, 14, f);
    fwrite(bmpInfoHeader, 1, 40, f);

    // GL_RGB pixels are stored from bottom row to top row. 
    // BMP is also bottom-to-top, but BGR format.
    for (int i = 0; i < w * h; ++i) {
        unsigned char r = pixels[i * 3 + 0];
        unsigned char g = pixels[i * 3 + 1];
        unsigned char b = pixels[i * 3 + 2];
        fwrite(&b, 1, 1, f);
        fwrite(&g, 1, 1, f);
        fwrite(&r, 1, 1, f);
    }

    fclose(f);
    std::cout << "[Lithium Engine] Screenshot saved successfully: " << filepath << std::endl;
}
Actor* Engine::spawn_actor_by_id(int id) {
    if (id == 0) return spawn_actor<EditorPrimitiveActor>("SpawnedCube", "Cube");
    if (id == 1) return spawn_actor<EditorPrimitiveActor>("SpawnedSphere", "Sphere");
    if (id == 2) return spawn_actor<DirectionalLightActor>("SpawnedLight");
    if (id == 3) {
        auto a = spawn_actor<Actor>("SpawnedParticles");
        a->create_component<ParticleEmitterComponent>("Emitter");
        return a;
    }
    if (id == 4) return spawn_actor<StaticSLRActor>("SpawnedLightRay");
    return spawn_actor<EditorPrimitiveActor>("SpawnedObject", "Cube");
}

void Engine::queue_destroy_actor(Actor* actor) {
    if (actor) {
        actors_to_destroy.push_back(actor);
    }
}

void Engine::script_request_spawn(int kind, double x, double y, double z) {
    std::lock_guard<std::mutex> lock(script_command_mutex);
    script_spawn_requests.push_back({ kind, x, y, z });
}

void Engine::script_request_destroy(Actor* actor) {
    if (!actor) return;
    std::lock_guard<std::mutex> lock(script_command_mutex);
    script_destroy_requests.push_back(actor);
}

void Engine::apply_script_commands() {
    std::vector<ScriptSpawnRequest> spawns;
    std::vector<Actor*> destroys;
    {
        // Swapped out under the lock rather than acted on in place: spawning runs
        // begin_play, which can itself request more scene changes.
        std::lock_guard<std::mutex> lock(script_command_mutex);
        if (script_spawn_requests.empty() && script_destroy_requests.empty()) return;
        spawns.swap(script_spawn_requests);
        destroys.swap(script_destroy_requests);
    }

    for (const ScriptSpawnRequest& request : spawns) {
        if (Actor* spawned = spawn_actor_by_id(request.kind)) {
            spawned->get_actor_transform().position = { request.x, request.y, request.z };
        }
    }
    for (Actor* actor : destroys) {
        queue_destroy_actor(actor);
    }
}

void Engine::destroy_queued_actors() {
    if (actors_to_destroy.empty()) return;
    for (auto* actor_to_destroy : actors_to_destroy) {
        actors.erase(
            std::remove_if(actors.begin(), actors.end(),
                [actor_to_destroy](const std::shared_ptr<Actor>& a) {
                    return a.get() == actor_to_destroy;
                }),
            actors.end()
        );
    }
    actors_to_destroy.clear();
}
