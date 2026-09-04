#include <cstdlib>
#include <algorithm>
#include "core/editor.hpp"
#include <functional>
#include "core/external_editor.hpp"
#include "core/platform.hpp"
#include <nlohmann/json.hpp>
#include "core/engine.hpp"
#include "world/cpp_script_component.hpp"
#include "world/light_components.hpp"
#include "world/camera_component.hpp"
#include "world/editor_primitive_actor.hpp"
#include "world/directional_light_actor.hpp"
#include "world/sprite_actor.hpp"
#include "imgui.h"
#include "ImGuizmo.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include "world/static_mesh_component.hpp"
#include "world/animation_player.hpp"
#include "world/joint_component.hpp"
#include "world/ui_canvas_component.hpp"
#include "world/lod_group_component.hpp"
#include "core/mesh_simplifier.hpp"
#include "world/nav_agent_component.hpp"
#include "navigation/navmesh.hpp"
#include "world/terrain_component.hpp"
#include "physics/physics_engine.hpp"
#include "audio/audio_engine.hpp"
#include "world/particle_emitter_component.hpp"
#include "world/lua_script_component.hpp"
#include "renderer/material_shader.hpp"
#include "scripting/cminus_interpreter.hpp"
#include "scripting/lua_api.hpp"
#include "world/lua_script_component.hpp"
#include "renderer/renderer.hpp"
#include "core/model_importer.hpp"
#include "core/scene_serializer.hpp"
#include "core/input_map.hpp"
#include "portable-file-dialogs.h"
#include "world/static_slr_actor.hpp"
#include "world/physics_attribute.hpp"
#include "world/character_controller_component.hpp"
#include "core/resource_manager.hpp"
#include "core/mesh_resource.hpp"
#include "world/audio_component.hpp"
#include "network/network_manager.hpp"
Editor::Editor() {
    visual_script_editor.initialize();
    load_preferences();
}

Editor::~Editor() {}

namespace {

// One layer picker, shared by every component that owns a body. Shows the layer's
// name rather than its number, because a matrix full of "Layer 7" helps nobody.
void draw_layer_combo(const char* label, int& layer) {
    std::string preview = std::to_string(layer) + ": " + PhysicsEngine::get_layer_name(layer);
    if (ImGui::BeginCombo(label, preview.c_str())) {
        for (int candidate = 0; candidate < PhysicsEngine::kLayerCount; ++candidate) {
            std::string entry = std::to_string(candidate) + ": " + PhysicsEngine::get_layer_name(candidate);
            const bool selected = (candidate == layer);
            if (ImGui::Selectable(entry.c_str(), selected)) layer = candidate;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

} // namespace

bool Editor::is_actor_selected(Actor* actor) const {
    for (Actor* a : selected_actors) {
        if (a == actor) return true;
    }
    return false;
}

// --- Prefabs ----------------------------------------------------------------

bool Editor::is_prefab_file(const std::string& path) {
    return path.size() >= 7 && path.compare(path.size() - 7, 7, ".prefab") == 0;
}

// Written into Content/ so the asset shows up in the content browser straight away,
// with no import step.
bool Editor::create_prefab_from_actor(Actor* actor) {
    if (!actor) return false;
    std::error_code ec;
    std::filesystem::create_directories("Content", ec);

    // Never silently overwrite a different prefab that happens to share a name -
    // actor names are not unique, so this would otherwise clobber earlier work.
    std::string base = "Content/" + actor->get_name();
    std::string path = base + ".prefab";
    for (int n = 1; std::filesystem::exists(path) && n < 1000; ++n) {
        path = base + "_" + std::to_string(n) + ".prefab";
    }

    if (!SceneSerializer::save_prefab(path, actor)) {
        std::cerr << "[Prefab] Failed to write " << path << std::endl;
        return false;
    }
    // The actor the prefab was made from becomes the first instance of it, which is
    // what makes "make a prefab, keep editing, apply" work at all.
    actor->prefab_source = path;
    std::cout << "[Prefab] Saved " << path << std::endl;
    return true;
}

Actor* Editor::instantiate_prefab(std::vector<std::shared_ptr<Actor>>& actors, const std::string& filepath) {
    auto instance = SceneSerializer::load_prefab(filepath);
    if (!instance) return nullptr;

    // The link back to the source. Without it the instance is just a copy, and
    // editing the prefab later would leave it behind.
    instance->prefab_source = filepath;

    // Dropped in front of the viewport camera rather than at the prefab's authored
    // position, which would often be off screen or on top of the original.
    instance->get_actor_transform().position = { 0.0, 0.0, -4.5 };

    Actor* raw = instance.get();
    actors.push_back(std::move(instance));
    clear_selection();
    select_actor(raw);
    record_scene_addition({ raw });
    scene_dirty = true;
    std::cout << "[Prefab] Instantiated " << filepath << std::endl;
    return raw;
}

void Editor::undo() {
    if (!undo_stack.empty()) {
        undo_stack.back()->undo();
        redo_stack.push_back(std::move(undo_stack.back()));
        undo_stack.pop_back();
        prune_selection();
        scene_dirty = true;
    }
}

void Editor::redo() {
    if (!redo_stack.empty()) {
        redo_stack.back()->redo();
        undo_stack.push_back(std::move(redo_stack.back()));
        redo_stack.pop_back();
        prune_selection();
        scene_dirty = true;
    }
}

void Editor::push_command(std::unique_ptr<Command> cmd) {
    if (!cmd) return;
    redo_stack.clear();
    undo_stack.push_back(std::move(cmd));
    scene_dirty = true;
}

void Editor::prune_selection() {
    if (!active_actors) return;
    selected_actors.erase(
        std::remove_if(selected_actors.begin(), selected_actors.end(), [this](Actor* a) {
            for (const auto& sp : *active_actors) {
                if (sp.get() == a) return false;
            }
            return true;
        }),
        selected_actors.end());
}

// Called after the actors are already in the scene list, so their indices are known.
void Editor::record_scene_addition(const std::vector<Actor*>& added) {
    if (!active_actors || added.empty()) return;
    std::vector<SceneMutationCommand::Entry> entries;
    for (size_t i = 0; i < active_actors->size(); ++i) {
        Actor* raw = (*active_actors)[i].get();
        if (std::find(added.begin(), added.end(), raw) != added.end()) {
            entries.push_back({ i, (*active_actors)[i] });
        }
    }
    if (!entries.empty()) {
        push_command(std::make_unique<SceneMutationCommand>(active_actors, std::move(entries), true));
    }
}

// Must be called *before* the actors are erased: it needs to read their indices and
// take a shared_ptr reference while they are still in the scene list.
void Editor::record_scene_removal(const std::vector<Actor*>& removed) {
    if (!active_actors || removed.empty()) return;
    std::vector<SceneMutationCommand::Entry> entries;
    for (size_t i = 0; i < active_actors->size(); ++i) {
        Actor* raw = (*active_actors)[i].get();
        if (std::find(removed.begin(), removed.end(), raw) != removed.end()) {
            entries.push_back({ i, (*active_actors)[i] });
        }
    }
    if (!entries.empty()) {
        push_command(std::make_unique<SceneMutationCommand>(active_actors, std::move(entries), false));
    }
}

void Editor::begin_property_edit() {
    if (property_edit_active) return;
    property_edit_start.clear();
    for (Actor* a : selected_actors) property_edit_start.push_back(ActorPropertyState::capture(a));
    property_edit_active = true;
}

void Editor::end_property_edit() {
    if (!property_edit_active) return;
    property_edit_active = false;

    std::vector<ActorPropertyState> after;
    for (const auto& before : property_edit_start) after.push_back(ActorPropertyState::capture(before.actor));

    bool changed = false;
    for (size_t i = 0; i < after.size() && i < property_edit_start.size(); ++i) {
        if (after[i].differs_from(property_edit_start[i])) { changed = true; break; }
    }
    // A click that merely focused a field changes nothing and must not push an entry,
    // otherwise Ctrl+Z appears to do nothing while it burns through empty commands.
    if (changed) {
        push_command(std::make_unique<PropertyCommand>(property_edit_start, std::move(after)));
    }
    property_edit_start.clear();
}

// Formats assimp can import, plus the engine's own cached .mesh.
bool Editor::is_model_file(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kModelExts[] = {
        ".mesh", ".gltf", ".glb", ".fbx", ".obj", ".dae", ".ply", ".stl", ".3ds"
    };
    for (const char* e : kModelExts) {
        if (ext == e) return true;
    }
    return false;
}

// Imports a model if needed and spawns it as a selected actor in front of the camera.
//
// Source formats are converted to the engine's .mesh once and cached next to the
// original, so re-importing the same file later skips assimp entirely.
Actor* Editor::spawn_model_actor(std::vector<std::shared_ptr<Actor>>& actors, const std::string& filepath) {
    namespace fs = std::filesystem;
    std::string mesh_path = filepath;

    if (fs::path(filepath).extension() != ".mesh") {
        std::string cached = "Content/" + fs::path(filepath).stem().string() + ".mesh";
        if (!fs::exists(cached)) {
            std::cout << "[Import] Converting " << filepath << " -> " << cached << std::endl;
            std::string produced = ModelImporter::import_model(filepath);
            if (produced.empty()) {
                std::cerr << "[Import] Failed to import " << filepath << std::endl;
                return nullptr;
            }
            mesh_path = produced;
        } else {
            mesh_path = cached;
        }
        // Watch the original so edits in the authoring tool re-import automatically.
        ResourceManager::get().watch_model_source(filepath, mesh_path);
    }

    if (!fs::exists(mesh_path)) {
        std::cerr << "[Import] Mesh missing after import: " << mesh_path << std::endl;
        return nullptr;
    }

    std::string act_name = fs::path(mesh_path).stem().string() + "_" + std::to_string(spawn_count++);
    auto new_actor = std::make_unique<Actor>(act_name);
    new_actor->shape_type = "StaticMesh";
    new_actor->mesh_path = mesh_path;
    auto* mesh_comp = new_actor->create_component<StaticMeshComponent>("Mesh");
    mesh_comp->set_mesh_resource(ResourceManager::get().load_async<MeshResource>(mesh_path));
    new_actor->set_root_component(mesh_comp);
    // Place it in front of wherever the camera is, not at the world origin, so it
    // lands in view rather than somewhere off screen.
    if (g_engine) {
        const DVector3& cam = g_engine->get_camera_position();
        new_actor->get_actor_transform().position = { cam.x, cam.y, cam.z - 5.0 };
    } else {
        new_actor->get_actor_transform().position = { 0.0, 0.0, -5.0 };
    }
    new_actor->begin_play();

    Actor* raw = new_actor.get();
    clear_selection();
    select_actor(raw);
    scene_dirty = true;
    actors.push_back(std::move(new_actor));
    record_scene_addition({ raw });
    std::cout << "[Import] Spawned " << act_name << std::endl;
    return raw;
}

// Build configuration dialog. Opened from Build > Build for <platform>, and gathers
// the settings that get baked into the packaged game's engine_config.json before the
// output folder is chosen.
void Editor::draw_build_dialog(std::vector<std::shared_ptr<Actor>>& actors) {
    if (!show_build_dialog) return;

    ImGui::OpenPopup("Build Settings");
    ImVec2 centre(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(centre, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Always);

    if (!ImGui::BeginPopupModal("Build Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }

    const bool windows_target = (build_settings.target_platform == 1);
    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Target: %s",
                       windows_target ? "Windows (.exe)" : "Linux (ELF)");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    ImGui::Text("Product");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##product", build_settings.product_name, sizeof(build_settings.product_name));

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "Display");
    ImGui::Separator();
    static const char* resolutions[] = { "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160" };
    ImGui::Combo("Resolution", &build_settings.resolution_index, resolutions, 5);
    ImGui::Checkbox("Fullscreen", &build_settings.fullscreen);
    ImGui::SameLine();
    ImGui::Checkbox("VSync", &build_settings.vsync);

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "Graphics");
    ImGui::Separator();
    static const char* presets[] = { "Low", "Medium", "High", "Ultra" };
    if (ImGui::Combo("Quality Preset", &build_settings.quality_preset, presets, 4)) {
        // Presets are just starting points; each toggle stays individually editable.
        switch (build_settings.quality_preset) {
            case 0: build_settings.enable_ssr = false; build_settings.enable_bloom = false;
                    build_settings.enable_taa = false; build_settings.enable_gi = false; break;
            case 1: build_settings.enable_ssr = false; build_settings.enable_bloom = true;
                    build_settings.enable_taa = true;  build_settings.enable_gi = true;  break;
            default: build_settings.enable_ssr = true; build_settings.enable_bloom = true;
                    build_settings.enable_taa = true;  build_settings.enable_gi = true;  break;
        }
    }
    ImGui::Checkbox("Screen Space Reflections", &build_settings.enable_ssr);
    ImGui::Checkbox("Bloom", &build_settings.enable_bloom);
    ImGui::Checkbox("Temporal Anti-Aliasing", &build_settings.enable_taa);
    ImGui::Checkbox("Global Illumination", &build_settings.enable_gi);

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "Packaging");
    ImGui::Separator();
    ImGui::Checkbox("Include sky library (adds ~300 MB)", &build_settings.include_editor_content);
    ImGui::Checkbox("Strip debug symbols", &build_settings.strip_debug);

    if (windows_target) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f),
                           "Requires a Windows runtime built beside the editor");
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f),
                           "(Lithium_Game.exe). See docs/Windows_Build_Guide.md");
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    if (ImGui::Button("Choose Folder and Build...", ImVec2(260, 34))) {
        auto selection = pfd::select_folder("Select Build Output Directory", ".").result();
        if (!selection.empty()) {
            std::string report;
            bool ok = run_build(actors, selection, report);
            pfd::message(ok ? "Build Complete" : "Build Finished With Problems",
                         report, pfd::choice::ok,
                         ok ? pfd::icon::info : pfd::icon::warning);
            show_build_dialog = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 34))) {
        show_build_dialog = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

// Assembles a runnable standalone game in out_dir from the current scene plus the
// chosen build settings. Uses std::filesystem throughout so it behaves the same on
// both host platforms.
bool Editor::run_build(std::vector<std::shared_ptr<Actor>>& actors, const std::string& out_dir, std::string& out_report) {
    namespace fs = std::filesystem;
    const bool windows_target = (build_settings.target_platform == 1);
    const std::string runtime = windows_target ? "Lithium_Game.exe" : "Lithium_Game";

    fs::path dest(out_dir);
    std::error_code ec;
    fs::create_directories(dest, ec);

    std::string problems;
    int copied = 0;

    if (!fs::exists(runtime)) {
        out_report = "Cannot build: the runtime '" + runtime + "' was not found next to the editor.\n\n";
        out_report += windows_target
            ? "Cross-compile it first (see docs/Windows_Build_Guide.md), then copy\nLithium_Game.exe beside the editor."
            : "Build the Lithium_Game target first.";
        return false;
    }
    fs::copy_file(runtime, dest / runtime, fs::copy_options::overwrite_existing, ec);
    if (ec) { problems += "  - runtime: " + ec.message() + "\n"; ec.clear(); } else { ++copied; }

    // The scene the game boots into.
    // save_scene returns void, so verify by checking the file landed.
    SceneSerializer::save_scene((dest / "project.lithium").string(), actors,
                                std::vector<std::string>(outliner_folders.begin(),
                                                         outliner_folders.end()));
    if (!fs::exists(dest / "project.lithium")) {
        problems += "  - scene could not be written\n";
    }

    // Native C++ scripts are built here rather than in the shipped game. Compiling
    // on the player's machine would make a C++ toolchain a requirement for running
    // the game, and the exported directory has no engine headers to compile
    // against anyway - so every script the project uses is turned into a module up
    // front and the runtime only loads it.
    {
        std::vector<std::string> scripts;
        for (const auto& actor : actors) {
            if (!actor) continue;
            if (auto* cs = actor->get_component<CppScriptComponent>()) {
                if (!cs->script_path.empty() &&
                    std::find(scripts.begin(), scripts.end(), cs->script_path) == scripts.end()) {
                    scripts.push_back(cs->script_path);
                }
            }
        }

        if (!scripts.empty()) {
            const fs::path module_dir = dest / CppScriptComponent::kModuleDir;
            fs::create_directories(module_dir, ec);
            ec.clear();

            // Cross-compiling a script for Windows needs the MinGW toolchain, which
            // the host compiler is not. Rather than emit modules the target cannot
            // load, say so and name the scripts affected.
            if (windows_target) {
                problems += "  - " + std::to_string(scripts.size()) +
                            " native C++ script(s) were not built: cross-compiling them for\n"
                            "    Windows requires a MinGW toolchain. Use C-Minus or Lua for\n"
                            "    scripts that must ship in a Windows build.\n";
            } else {
                for (const std::string& script : scripts) {
                    const fs::path out_module =
                        module_dir / CppScriptComponent::module_name_for(script);
                    std::string log;
                    if (CppScriptComponent::compile_script(script, out_module.string(), log)) {
                        ++copied;
                    } else {
                        problems += "  - script " + script + ":\n";
                        // The compiler's own diagnostics are the useful part; indent
                        // them so they read as belonging to this entry.
                        std::istringstream lines(log);
                        std::string line;
                        while (std::getline(lines, line)) {
                            if (!line.empty()) problems += "      " + line + "\n";
                        }
                    }
                }
            }
        }
    }

    // Runtime payload. lib/ only matters for the Linux target; Windows keeps its DLLs
    // beside the executable, so those are picked up by the loop below instead.
    std::vector<std::string> payload = { "Content", "shaders", "assets" };
    if (build_settings.include_editor_content) payload.push_back("EngineContent");
    if (!windows_target) payload.push_back("lib");
    for (const std::string& dir : payload) {
        if (!fs::exists(dir)) continue;
        fs::copy(dir, dest / dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) { problems += "  - " + dir + ": " + ec.message() + "\n"; ec.clear(); } else { ++copied; }
    }
    if (windows_target) {
        for (const auto& entry : fs::directory_iterator(".", ec)) {
            if (entry.path().extension() == ".dll") {
                fs::copy_file(entry.path(), dest / entry.path().filename(),
                              fs::copy_options::overwrite_existing, ec);
                if (ec) ec.clear(); else ++copied;
            }
        }
        ec.clear();
    }

    // Bake the chosen settings into the game's config so it launches configured.
    static const int res_w[] = { 1280, 1600, 1920, 2560, 3840 };
    static const int res_h[] = { 720, 900, 1080, 1440, 2160 };
    int ri = build_settings.resolution_index;
    if (ri < 0 || ri > 4) ri = 0;
    try {
        nlohmann::json cfg;
        cfg["graphics_api"] = "opengl";
        cfg["product_name"] = std::string(build_settings.product_name);
        cfg["resolution_width"] = res_w[ri];
        cfg["resolution_height"] = res_h[ri];
        cfg["fullscreen"] = build_settings.fullscreen;
        cfg["vsync"] = build_settings.vsync;
        cfg["enable_ssr"] = build_settings.enable_ssr;
        cfg["enable_bloom"] = build_settings.enable_bloom;
        cfg["enable_taa"] = build_settings.enable_taa;
        cfg["gi_enabled"] = build_settings.enable_gi;
        std::ofstream out(dest / "engine_config.json");
        out << cfg.dump(4);
    } catch (const std::exception& e) {
        problems += std::string("  - config: ") + e.what() + "\n";
    }

    // A launcher for the target platform, not the host one.
    if (windows_target) {
        std::ofstream bat(dest / "launch.bat");
        if (bat.is_open()) {
            bat << "@echo off\r\n";
            bat << "cd /d \"%~dp0\"\r\n";
            bat << runtime << " project.lithium\r\n";
        }
    } else {
        fs::path sh = dest / "launch.sh";
        std::ofstream script(sh);
        if (script.is_open()) {
            script << "#!/bin/bash\n";
            script << "cd \"$(dirname \"$(readlink -f \"$0\")\")\" || exit 1\n";
            script << "export LD_LIBRARY_PATH=\"$PWD/lib:$LD_LIBRARY_PATH\"\n";
            script << "./" << runtime << " project.lithium\n";
            script.close();
            fs::permissions(sh, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, ec);
            ec.clear();
        }
    }

    if (build_settings.strip_debug && !windows_target) {
        // Best effort; a missing strip is not a build failure.
        std::string cmd = "strip \"" + (dest / runtime).string() + "\" 2>/dev/null";
        std::system(cmd.c_str());
    }

    out_report = std::string(build_settings.product_name) + " built for " +
                 (windows_target ? "Windows" : "Linux") + "\n\n" + dest.string() +
                 "\n\n" + std::to_string(copied) + " item(s) packaged.";
    if (!problems.empty()) {
        out_report += "\n\nThese did not copy:\n" + problems;
        return false;
    }
    return true;
}

bool Editor::save_scene(std::vector<std::shared_ptr<Actor>>& actors) {
    if (current_scene_path.empty()) {
        auto f = pfd::save_file("Save Scene", ".", { "Lithium Scene Files", "*.lithium" });
        if (f.result().empty()) return false;   // user cancelled
        current_scene_path = f.result();
        if (current_scene_path.find(".lithium") == std::string::npos) current_scene_path += ".lithium";
    }
    SceneSerializer::save_scene(current_scene_path, actors,
                                std::vector<std::string>(outliner_folders.begin(),
                                                         outliner_folders.end()));
    scene_dirty = false;
    return true;
}

EditorRequest Editor::render(std::vector<std::shared_ptr<Actor>>& actors, unsigned int viewport_texture_id, unsigned int logo_texture_id, bool& out_screenshot_requested, bool& out_viewport_clicked, float& out_ndc_x, float& out_ndc_y, const Matrix4x4& view, const Matrix4x4& proj, Renderer* renderer, EngineState engine_state) {
    active_renderer = renderer;
    EditorRequest req = EditorRequest::None;
    ImGuizmo::BeginFrame();
    
    if (actors.empty()) {
        spawn_count = 0;
    }

    // Editor chords are suppressed while a widget owns the keyboard, so typing in a
    // property field cannot trigger save/undo/redo.
    const bool shortcuts_allowed = !ImGui::GetIO().WantCaptureKeyboard;

    if (shortcuts_allowed && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        save_scene(actors);
    }

    if (shortcuts_allowed && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (ImGui::GetIO().KeyShift) {
            redo();
        } else {
            undo();
        }
    }
    
    if (shortcuts_allowed && ImGui::GetIO().KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y, false) || ImGui::IsKeyPressed(ImGuiKey_R, false))) {
        redo();
    }

    // Setup full screen workspace dockspace
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;

    ImGui::Begin("Lithium_Engine Workspace", nullptr, window_flags);

    // FPS Overlay
    float fps = ImGui::GetIO().Framerate;
    ImU32 fps_color = IM_COL32(255, 0, 0, 255); // Red for < 30
    if (fps >= 101.0f) {
        fps_color = IM_COL32(160, 32, 240, 255); // Purple
    } else if (fps >= 60.0f) {
        fps_color = IM_COL32(0, 255, 0, 255); // Green
    } else if (fps >= 30.0f) {
        fps_color = IM_COL32(255, 255, 0, 255); // Yellow
    }
    char fps_text[32];
    snprintf(fps_text, sizeof(fps_text), "FPS: %.1f", fps);
    // Parked at the right-hand end of the menu bar row, which is empty. It used to sit
    // at a fixed (10, 40) - directly on top of the toolbar's left edge, where the gizmo
    // mode buttons now are.
    ImVec2 fps_size = ImGui::CalcTextSize(fps_text);
    ImGui::GetForegroundDrawList()->AddText(
        ImVec2(ImGui::GetIO().DisplaySize.x - fps_size.x - 14.0f, 6.0f), fps_color, fps_text);

    draw_menu_bar(actors, out_screenshot_requested);
    
    // Undo/redo and the scene-mutation commands operate on the live actor list.
    active_actors = &actors;

    // Draw Toolbar for PIE
    EditorRequest toolbar_req = draw_toolbar(engine_state);
    if (toolbar_req != EditorRequest::None) {
        req = toolbar_req;
    }

    // Top section: Outliner (Left), Viewport (Center), Properties (Right)
    float window_width = ImGui::GetContentRegionAvail().x;
    float window_height = ImGui::GetContentRegionAvail().y;

    ImGui::BeginChild("TopRow", ImVec2(0.0f, window_height - 180.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
    float top_height = ImGui::GetContentRegionAvail().y;

    // Laid out as a table rather than three sibling children. ImGuiChildFlags_ResizeX
    // only grips a child's *right* border, so the last panel on a row - Details here,
    // the Spawner below - could never be resized: there was nothing to its right to
    // drag. Table columns put a draggable separator between every pane instead.
    const ImGuiTableFlags pane_table_flags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;

    if (ImGui::BeginTable("TopPanes", 3, pane_table_flags, ImVec2(0.0f, top_height))) {
        ImGui::TableSetupColumn("Outliner", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("Viewport", ImGuiTableColumnFlags_WidthStretch, 0.53f);
        ImGui::TableSetupColumn("Details",  ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableNextRow();

    // Outliner (Left Panel)
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginChild("OutlinerPanel", ImVec2(0.0f, top_height), ImGuiChildFlags_Borders);
    if (logo_texture_id != 0) {
        // Fit the logo to a fixed height, deriving width from its real aspect, and
        // centre it in the panel instead of stretching it to fill a fixed box.
        float logo_height = 56.0f;
        float logo_width = logo_height * logo_aspect;
        float avail = ImGui::GetContentRegionAvail().x;
        if (logo_width > avail) {
            logo_width = avail;
            logo_height = (logo_aspect > 0.0f) ? logo_width / logo_aspect : logo_height;
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - logo_width) * 0.5f));
        ImGui::Image((void*)(intptr_t)logo_texture_id, ImVec2(logo_width, logo_height));
        ImGui::Separator();
    }
    draw_outliner(actors);
    ImGui::EndChild();

    // Viewport (Center Panel)
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("ViewportPanel", ImVec2(0.0f, top_height), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("ViewportTabs")) {
        if (ImGui::BeginTabItem("Viewport")) {
            draw_viewport(actors, viewport_texture_id, out_viewport_clicked, out_ndc_x, out_ndc_y, view, proj, renderer);
            if (renderer && renderer->is_offline_rendering) {
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                ImVec2 pos = ImGui::GetItemRectMin();
                pos.x += 10.0f;
                pos.y += 10.0f;
                int samples = renderer->get_offline_sample_count();
                int target = renderer->get_offline_target_samples();
                char overlay_text[256];
                if (!renderer->is_offline_complete()) {
                    snprintf(overlay_text, sizeof(overlay_text), "TESLA (%s) - %d / %d samples",
                             renderer->offline_backend_name(), samples, target);
                } else {
                    snprintf(overlay_text, sizeof(overlay_text), "TESLA (%s) - converged, %d samples",
                             renderer->offline_backend_name(), samples);
                }
                draw_list->AddText(pos, IM_COL32(255, 255, 0, 255), overlay_text);
            }
            ImGui::EndTabItem();
        }
        draw_preferences();
        if (show_script_editor) {
            bool open = true;
            if (ImGui::BeginTabItem("Script Editor", &open)) {
                draw_script_editor();
                ImGui::EndTabItem();
            }
            if (!open) show_script_editor = false;
        }
        if (show_visual_script_editor) {
            bool open = true;
            if (ImGui::BeginTabItem("Visual Script Editor", &open)) {
                visual_script_editor.render(selected_actors);
                ImGui::EndTabItem();
            }
            if (!open) show_visual_script_editor = false;
        }
        if (show_material_editor) {
            bool open = true;
            if (ImGui::BeginTabItem("Material Editor", &open)) {
                draw_material_editor();
                ImGui::EndTabItem();
            }
            if (!open) show_material_editor = false;
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    // Properties (Right Panel)
    ImGui::TableSetColumnIndex(2);
    ImGui::BeginChild("PropertiesPanel", ImVec2(0.0f, top_height), ImGuiChildFlags_Borders);
    // A property widget becoming active inside the Details panel means the user has
    // started editing the selected actor, so the scene now differs from what is on
    // disk. This errs toward marking dirty: a spurious "unsaved changes" prompt costs
    // one click, a missed one costs the user their work.
    bool any_item_active_before_details = ImGui::IsAnyItemActive();
    draw_properties();
    if (!any_item_active_before_details && ImGui::IsAnyItemActive()) {
        scene_dirty = true;
    }
    // Bracket the edit around widget activation rather than around each field. A
    // slider drag spans hundreds of frames, and one undo entry per frame would make
    // Ctrl+Z useless; this captures on the frame a widget goes active and commits a
    // single diff once it is released.
    if (ImGui::IsAnyItemActive()) {
        begin_property_edit();
    } else {
        end_property_edit();
    }
    ImGui::EndChild();

        ImGui::EndTable();
    }

    ImGui::EndChild(); // End TopRow

    // Bottom section: Content Browser (Left-Center), Spawner (Right)
    ImGui::Dummy(ImVec2(0.0f, 5.0f));

    ImGui::BeginChild("BottomRow", ImVec2(0.0f, 0.0f), 0);
    float bottom_height = ImGui::GetContentRegionAvail().y;

    // Same reasoning as the top row: the Spawner is last on the row, so it needs a
    // table column separator to its left to be resizable at all.
    if (ImGui::BeginTable("BottomPanes", 2, pane_table_flags, ImVec2(0.0f, bottom_height))) {
        ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch, 0.75f);
        ImGui::TableSetupColumn("Spawner", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("BottomPanel", ImVec2(0.0f, bottom_height), ImGuiChildFlags_Borders);
        if (ImGui::BeginTabBar("BottomTabs")) {
            if (ImGui::BeginTabItem("Content Browser")) {
                draw_content_browser(renderer);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Profiler")) {
                draw_profiler(renderer, actors);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("SpawnerPanel", ImVec2(0.0f, bottom_height), ImGuiChildFlags_Borders);
        draw_spawner(actors);
        draw_build_dialog(actors);
        ImGui::EndChild();

        ImGui::EndTable();
    }

    ImGui::EndChild(); // End BottomRow

    ImGui::End();

    // Its own top-level window, so it is submitted after the workspace one ends.
    draw_input_bindings();
    draw_navigation_window(actors);
    draw_collision_layers();
    draw_audio_buses();
    draw_lighting_bake(actors);

    return req;
}

EditorRequest Editor::draw_toolbar(EngineState engine_state) {
    EditorRequest req = EditorRequest::None;
    
    ImGui::BeginChild("ToolbarPanel", ImVec2(0.0f, 40.0f), true);

    // Gizmo mode selector. The rotate and scale gizmos were only ever reachable
    // through Ctrl+E / Ctrl+R, with nothing on screen to say they existed, so in
    // practice only the translate gizmo got used. Measure the full width before
    // drawing them: the Play/Pause/Stop group is centred against the whole toolbar,
    // not against whatever space these leave behind.
    float full_width = ImGui::GetContentRegionAvail().x;

    const char* mode_labels[3] = { "Move", "Rotate", "Scale" };
    const char* mode_tips[3] = { "Translate  (Ctrl+W)", "Rotate  (Ctrl+E)", "Scale  (Ctrl+R)" };
    for (int i = 0; i < 3; ++i) {
        bool active = (gizmo_mode == i);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.80f, 1.0f));
        if (ImGui::Button(mode_labels[i], ImVec2(62.0f, 25.0f))) gizmo_mode = i;
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", mode_tips[i]);
        ImGui::SameLine();
    }
    // Local vs world axes. Shows the space currently in effect, not the one a click
    // would switch to, so it reads as a status rather than a command.
    if (ImGui::Button(gizmo_local_space ? "Local" : "World", ImVec2(58.0f, 25.0f))) {
        gizmo_local_space = !gizmo_local_space;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Gizmo axis space  (Ctrl+S)");

    float button_width = 80.0f;
    float center_offset = (full_width - (button_width * 3 + 20.0f)) * 0.5f;
    // Only re-centre if there is actually room left of the group; otherwise just
    // continue on the same line so the buttons never overlap on a narrow window.
    ImGui::SameLine();
    if (center_offset > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(center_offset);
    }
    
    if (engine_state == EngineState::Editor) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
        if (ImGui::Button("Play", ImVec2(button_width, 25.0f))) req = EditorRequest::Play;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::BeginDisabled();
        ImGui::Button("Pause", ImVec2(button_width, 25.0f));
        ImGui::SameLine();
        ImGui::Button("Stop", ImVec2(button_width, 25.0f));
        ImGui::EndDisabled();
    } else if (engine_state == EngineState::PlayInEditor) {
        ImGui::BeginDisabled();
        ImGui::Button("Play", ImVec2(button_width, 25.0f));
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.7f, 0.2f, 1.0f));
        if (ImGui::Button("Pause", ImVec2(button_width, 25.0f))) req = EditorRequest::Pause;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Stop", ImVec2(button_width, 25.0f))) req = EditorRequest::Stop;
        ImGui::PopStyleColor();
    } else if (engine_state == EngineState::PlayInEditorPaused) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
        if (ImGui::Button("Resume", ImVec2(button_width, 25.0f))) req = EditorRequest::Resume;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::BeginDisabled();
        ImGui::Button("Pause", ImVec2(button_width, 25.0f));
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Stop", ImVec2(button_width, 25.0f))) req = EditorRequest::Stop;
        ImGui::PopStyleColor();
    }
    
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 320.0f);
    
    auto mode = NetworkManager::get().get_mode();
    if (mode == NetworkMode::Offline) {
        if (ImGui::Button("Host Server (7777)", ImVec2(140.0f, 25.0f))) {
            NetworkManager::get().host_server(7777);
        }
        ImGui::SameLine();
        if (ImGui::Button("Connect (127.0.0.1)", ImVec2(150.0f, 25.0f))) {
            NetworkManager::get().connect_to_server("127.0.0.1", 7777);
        }
    } else {
        ImGui::Text(mode == NetworkMode::Server ? "Status: Hosting" : "Status: Connected");
        ImGui::SameLine();
        if (ImGui::Button("Disconnect", ImVec2(100.0f, 25.0f))) {
            NetworkManager::get().disconnect();
        }
    }
    
    ImGui::EndChild();
    return req;
}

void Editor::draw_menu_bar(std::vector<std::shared_ptr<Actor>>& actors, bool& out_screenshot_requested) {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Files")) {
            if (ImGui::MenuItem("New Level")) {
                actors.clear();
                clear_selection();
            }
            if (ImGui::MenuItem("Save Level As...")) {
                auto f = pfd::save_file("Save Scene", ".", { "Lithium Scene Files", "*.lithium" });
                if (!f.result().empty()) {
                    current_scene_path = f.result();
                    if (current_scene_path.find(".lithium") == std::string::npos) current_scene_path += ".lithium";
                    SceneSerializer::save_scene(current_scene_path, actors,
                                std::vector<std::string>(outliner_folders.begin(),
                                                         outliner_folders.end()));
                    scene_dirty = false;
                }
            }
            if (ImGui::MenuItem("Load Level")) {
                auto f = pfd::open_file("Open Scene", ".", { "Lithium Scene Files", "*.lithium" });
                if (!f.result().empty()) {
                    current_scene_path = f.result()[0];
                    SceneSerializer::load_scene(current_scene_path, actors);
                    // Including folders that hold nothing, which no actor could
                    // have told us about.
                    outliner_folders.clear();
                    for (const std::string& folder : SceneSerializer::last_loaded_folders()) {
                        outliner_folders.insert(folder);
                    }
                    scene_dirty = false;
                    clear_selection();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Take Screenshot (F9)")) {
                out_screenshot_requested = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit Engine")) {}
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::BeginMenu("Project Settings")) {
                if (g_engine) {
                    ImGui::Checkbox("2D Game Mode", &g_engine->active_config.is_2d_mode);
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Input Bindings...")) show_input_bindings = true;
            if (ImGui::MenuItem("Navigation...")) show_navigation_window = true;
            if (ImGui::MenuItem("Collision Layers...")) show_collision_layers = true;
            if (ImGui::MenuItem("Audio Buses...")) show_audio_buses = true;
            if (ImGui::MenuItem("Preferences...")) show_preferences = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Rendering")) {
            // Also exposed here, not only in the Spawner Panel: baking is a top-level
            // scene operation and was buried below a scroll in a side panel.
            if (ImGui::MenuItem("Baked Lighting (Lightmaps + Probes)...")) {
                show_lighting_bake = true;
            }
            if (ImGui::MenuItem("Bake Static Lighting")) {
                if (g_engine) g_engine->bake_static_lighting();
            }
            if (active_renderer) {
                if (ImGui::BeginMenu("Global Illumination")) {
                    static const char* gi_names[] = {
                        "Off / Disabled", "Screen-Space GI (SSGI)",
                        "Voxel Cone Tracing (VXGI)", "Hardware Ray Tracing"
                    };
                    for (int i = 0; i < 4; ++i) {
                        bool sel = (static_cast<int>(active_renderer->gi_mode) == i);
                        if (ImGui::MenuItem(gi_names[i], NULL, sel)) {
                            active_renderer->gi_mode = static_cast<GIMode>(i);
                        }
                    }
                    ImGui::Separator();
                    ImGui::TextDisabled("VXGI / Hardware RT have no backend");
                    ImGui::TextDisabled("in this build; they run SSGI.");
                    ImGui::EndMenu();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Wireframe Mode", NULL, wireframe_mode)) {
                wireframe_mode = true;
                tesla_mode = false;
            }
            if (ImGui::MenuItem("Sodium Real-Time (Default)", NULL, !wireframe_mode && !tesla_mode)) {
                wireframe_mode = false;
                tesla_mode = false;
            }
            if (ImGui::MenuItem("TESLA Path Tracer", NULL, tesla_mode)) {
                tesla_mode = true;
                wireframe_mode = false;
            }
            if (active_renderer && ImGui::BeginMenu("TESLA Settings")) {
                TeslaSettings& ts = active_renderer->tesla.settings();

                if (active_renderer->tesla.gpu_available()) {
                    bool gpu = ts.use_gpu;
                    if (ImGui::Checkbox("Trace on the GPU", &gpu)) {
                        ts.use_gpu = gpu;
                        active_renderer->tesla.reset_accumulation();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Both backends run the same integrator and\n"
                                          "converge to the same image; the GPU one is\n"
                                          "simply faster.");
                    }
                } else {
                    ImGui::TextDisabled("GPU backend unavailable on this driver");
                }

                ImGui::Separator();

                int target = ts.target_samples;
                if (ImGui::SliderInt("Target samples", &target, 16, 8192)) {
                    ts.target_samples = target;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Noise falls as 1/sqrt(samples). Raising this after a\n"
                                      "render has finished resumes it rather than restarting.");
                }

                ImGui::Separator();

                // Denoising. Presented as a display option rather than a render
                // setting, because it is exactly that: the accumulation underneath
                // is untouched and unbiased, and turning this off shows it raw.
                if (active_renderer->tesla.denoise_available()) {
                    ImGui::Checkbox("Denoise (Open Image Denoise)", &ts.denoise);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Runs Intel Open Image Denoise over the finished\n"
                                          "image, using first-hit albedo and normals to keep\n"
                                          "edges the noisy estimate cannot resolve.\n\n"
                                          "The accumulation is not modified - this is a view\n"
                                          "of it, and unticking shows the raw result.");
                    }
                    if (!ts.denoise) ImGui::BeginDisabled();
                    ImGui::Checkbox("Denoise while rendering", &ts.denoise_while_rendering);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Filters every preview update instead of only the\n"
                                          "finished render. Costs more time than the samples\n"
                                          "it saves, and is worth it while dialling in lighting.");
                    }
                    if (!ts.denoise) ImGui::EndDisabled();

                    if (active_renderer->tesla.denoised_is_current()) {
                        ImGui::TextDisabled("Showing denoised image");
                    }
                } else {
                    ImGui::TextDisabled("Denoiser unavailable in this build");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Built without Open Image Denoise. Place the SDK in\n"
                                          "thirdparty/oidn and re-run CMake to enable it.");
                    }
                }

                ImGui::Separator();

                ImGui::Checkbox("Auto exposure", &active_renderer->tesla_auto_exposure);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("The rasteriser's auto-exposure is driven by a buffer the\n"
                                      "geometry pass fills in, and that pass does not run here.\n"
                                      "This is TESLA's own equivalent; without it a dim scene\n"
                                      "tonemaps to black.");
                }
                ImGui::SliderFloat(active_renderer->tesla_auto_exposure ? "Exposure bias" : "Exposure",
                                   &active_renderer->tesla_exposure, 0.05f, 8.0f, "%.2f");

                ImGui::Separator();

                int max_depth = ts.max_depth;
                if (ImGui::SliderInt("Max bounces (0 = unlimited)", &max_depth, 0, 64)) {
                    ts.max_depth = max_depth;
                    active_renderer->tesla.reset_accumulation();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Russian roulette already ends paths without bias.\n"
                                      "A hard cap re-introduces the truncation bias it\n"
                                      "exists to avoid, so leave this at 0 unless a scene\n"
                                      "genuinely needs it.");
                }

                float clamp_value = ts.firefly_clamp;
                if (ImGui::SliderFloat("Firefly clamp (0 = off)", &clamp_value, 0.0f, 20.0f, "%.1f")) {
                    ts.firefly_clamp = clamp_value;
                    active_renderer->tesla.reset_accumulation();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Caps a single path's contribution. Any non-zero value\n"
                                      "biases the result - it trades correctness for fewer\n"
                                      "bright speckles.");
                }

                ImGui::Separator();
                ImGui::Text("Backend: %s", active_renderer->tesla.backend_name());
                ImGui::Text("Triangles: %d", active_renderer->tesla.triangle_count());
                ImGui::Text("Samples: %d / %d",
                            active_renderer->tesla.samples_done(), ts.target_samples);

                if (ImGui::MenuItem("Restart accumulation")) {
                    active_renderer->tesla.reset_accumulation();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Visual Script Editor")) {
                show_visual_script_editor = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Build")) {
            if (ImGui::MenuItem("Build for Linux...")) {
                build_settings.target_platform = 0;
                show_build_dialog = true;
            }
            if (ImGui::MenuItem("Build for Windows...")) {
                build_settings.target_platform = 1;
                show_build_dialog = true;
            }
            ImGui::Separator();
            ImGui::TextDisabled("Opens build settings, then asks");
            ImGui::TextDisabled("where to write the output.");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Export")) {
            if (ImGui::MenuItem("Quick Package (current platform)")) {
                // Shelling out to cp/chmod tied this to a Unix host. std::filesystem
                // does the same job portably and reports failures instead of silently
                // discarding them down /dev/null.
                if (!Platform::has_cpp_compiler()) {
                    pfd::message("Note",
                        "No C++ compiler found on PATH.\n\n"
                        "The exported game runs fine without one; a compiler is only\n"
                        "needed if your project uses native C++ scripting.",
                        pfd::choice::ok, pfd::icon::warning);
                }

                auto selection = pfd::select_folder("Select Export Directory", ".").result();
                if (!selection.empty()) {
                    namespace fs = std::filesystem;
                    fs::path target_dir(selection);
                    std::cout << "Exporting game to: " << target_dir.string() << std::endl;

                    std::error_code ec;
                    std::string problems;

                    const std::string exe_name = std::string("Lithium_Game") +
                        (LITHIUM_PLATFORM_WINDOWS ? ".exe" : "");
                    if (fs::exists(exe_name)) {
                        fs::copy_file(exe_name, target_dir / exe_name,
                                      fs::copy_options::overwrite_existing, ec);
                        if (ec) problems += "  - runtime binary: " + ec.message() + "\n";
                        ec.clear();
                    } else {
                        problems += "  - " + exe_name + " not found next to the editor\n";
                    }

                    if (!current_scene_path.empty() && fs::exists(current_scene_path)) {
                        fs::copy_file(current_scene_path, target_dir / "project.lithium",
                                      fs::copy_options::overwrite_existing, ec);
                        if (ec) problems += "  - scene: " + ec.message() + "\n";
                        ec.clear();
                    }

                    // Optional payload directories; absent ones are not an error.
                    for (const char* dir : { "lib", "assets", "shaders", "Content", "EngineContent" }) {
                        if (!fs::exists(dir)) continue;
                        fs::copy(dir, target_dir / dir,
                                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                        if (ec) { problems += std::string("  - ") + dir + ": " + ec.message() + "\n"; ec.clear(); }
                    }

                    // A launcher for whichever platform this editor is running on.
                    if (LITHIUM_PLATFORM_WINDOWS) {
                        std::ofstream script(target_dir / "launch.bat");
                        if (script.is_open()) {
                            script << "@echo off\r\n";
                            script << "cd /d \"%~dp0\"\r\n";
                            script << "Lithium_Game.exe project.lithium\r\n";
                        }
                    } else {
                        fs::path script_path = target_dir / "launch.sh";
                        std::ofstream script(script_path);
                        if (script.is_open()) {
                            script << "#!/bin/bash\n";
                            script << "cd \"$(dirname \"$(readlink -f \"$0\")\")\" || exit 1\n";
                            script << "export LD_LIBRARY_PATH=\"$PWD/lib:$LD_LIBRARY_PATH\"\n";
                            script << "./Lithium_Game project.lithium\n";
                            script.close();
                            fs::permissions(script_path,
                                fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                                fs::perm_options::add, ec);
                            ec.clear();
                        }
                    }

                    if (problems.empty()) {
                        pfd::message("Export Complete",
                                     "Packaged game to:\n" + target_dir.string(),
                                     pfd::choice::ok, pfd::icon::info);
                    } else {
                        pfd::message("Export Finished With Problems",
                                     "Packaged to:\n" + target_dir.string() +
                                     "\n\nThese items did not copy:\n" + problems,
                                     pfd::choice::ok, pfd::icon::warning);
                    }
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

namespace {

// Splits a trailing " (N)" off a name, so duplicating "Cube (2)" yields "Cube (3)"
// rather than "Cube (2) (1)". Only a run of digits in parentheses at the very end
// counts, so a legitimately-named "Barrel (Broken)" keeps its suffix.
std::string strip_duplicate_suffix(const std::string& name) {
    if (name.size() < 4 || name.back() != ')') return name;

    const size_t open = name.find_last_of('(');
    if (open == std::string::npos || open == 0) return name;
    // Exactly one space before the parenthesis is the shape this writes, and the
    // shape it will consume again.
    if (name[open - 1] != ' ') return name;

    for (size_t i = open + 1; i + 1 < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return name;
    }
    // "Cube ()" has no digits between the parentheses and is not a suffix.
    if (open + 1 >= name.size() - 1) return name;

    return name.substr(0, open - 1);
}

// First free name in the "stem", "stem (1)", "stem (2)" sequence.
//
// The base name itself is offered first: renaming something to a name nobody else
// holds must not gratuitously append "(1)".
std::string unique_name(const std::string& desired,
                        const std::function<bool(const std::string&)>& taken) {
    if (!taken(desired)) return desired;

    const std::string stem = strip_duplicate_suffix(desired);
    // Bounded so a pathological scene cannot spin here; 10000 of one name is
    // already far past anything anyone organises by hand.
    for (int i = 1; i < 10000; ++i) {
        const std::string candidate = stem + " (" + std::to_string(i) + ")";
        if (!taken(candidate)) return candidate;
    }
    return desired;
}

} // namespace

// =============================================================================
//  Outliner folders
//
//  Purely an editor-side organisation aid: a folder is a string on the actor, not
//  a node in the scene graph. Attaching actors to a real parent transform would
//  have moved them, which is emphatically not what "put these in a folder" means.
//  So folders have no effect on transforms, parenting, physics or gameplay - they
//  are exactly what a directory is in a file browser.
// =============================================================================

namespace {

// Parent of a folder path, or empty for a top-level one.
std::string folder_parent(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash);
}

// Last component of a folder path - what is actually shown on the row.
std::string folder_leaf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// True if `path` is `ancestor` or sits underneath it. The trailing slash matters:
// without it "Lights" would claim "LightsB" as a child.
bool is_within(const std::string& path, const std::string& ancestor) {
    if (ancestor.empty()) return true;
    if (path == ancestor) return true;
    return path.size() > ancestor.size() &&
           path.compare(0, ancestor.size(), ancestor) == 0 &&
           path[ancestor.size()] == '/';
}

// Strips characters that would break the path encoding or read as nesting the user
// did not ask for. A name is a single component; nesting is done by dragging.
std::string sanitize_folder_name(const std::string& name) {
    std::string cleaned;
    for (char c : name) {
        if (c == '/' || c == '\\') continue;
        cleaned += c;
    }
    // Trim, so a name of only spaces cannot produce an unclickable blank row.
    const size_t first = cleaned.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const size_t last = cleaned.find_last_not_of(" \t");
    return cleaned.substr(first, last - first + 1);
}

// Payload id for dragging an actor onto a folder.
constexpr const char* kActorDragType = "OUTLINER_ACTOR";

} // namespace

bool Editor::actor_matches_filter(const Actor* actor) const {
    if (outliner_filter[0] == '\0') return true;
    std::string filter_lower = outliner_filter;
    std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string label = actor->get_name() + " (" + actor->shape_type + ")";
    std::transform(label.begin(), label.end(), label.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return label.find(filter_lower) != std::string::npos;
}

void Editor::accept_actor_drop(const std::string& target_folder) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kActorDragType)) {
        Actor* dragged = *static_cast<Actor* const*>(payload->Data);
        if (dragged) {
            // The whole selection moves, not just the row under the cursor. Dragging
            // one of five selected actors and having the other four stay put is the
            // more surprising behaviour.
            if (is_actor_selected(dragged)) {
                for (Actor* actor : selected_actors) {
                    if (actor) actor->set_folder_path(target_folder);
                }
            } else {
                dragged->set_folder_path(target_folder);
            }
            scene_dirty = true;
        }
    }
    ImGui::EndDragDropTarget();
}

void Editor::rename_outliner_folder(const std::string& old_path, const std::string& new_name,
                                    std::vector<std::shared_ptr<Actor>>& actors) {
    const std::string cleaned = sanitize_folder_name(new_name);
    if (cleaned.empty() || old_path.empty()) return;

    const std::string parent = folder_parent(old_path);
    const std::string new_path = parent.empty() ? cleaned : parent + "/" + cleaned;
    if (new_path == old_path) return;
    // A rename onto an existing sibling would silently merge the two, which is not
    // what a rename means and cannot be undone by renaming back.
    if (outliner_folders.count(new_path)) return;

    // Re-file the folder itself and every folder nested inside it.
    std::set<std::string> updated;
    for (const std::string& folder : outliner_folders) {
        updated.insert(is_within(folder, old_path)
            ? new_path + folder.substr(old_path.size())
            : folder);
    }
    outliner_folders = std::move(updated);

    for (const auto& actor : actors) {
        if (actor && is_within(actor->get_folder_path(), old_path)) {
            actor->set_folder_path(new_path + actor->get_folder_path().substr(old_path.size()));
        }
    }
    scene_dirty = true;
}

void Editor::delete_outliner_folder(const std::string& path,
                                    std::vector<std::shared_ptr<Actor>>& actors) {
    if (path.empty()) return;
    const std::string parent = folder_parent(path);

    // Contents move up to the parent rather than being destroyed. Deleting a folder
    // is an organisational act; taking the actors with it would make it a
    // catastrophic one, and there is no confirmation dialog here to justify that.
    for (const auto& actor : actors) {
        if (!actor) continue;
        const std::string& folder = actor->get_folder_path();
        if (!is_within(folder, path)) continue;
        if (folder == path) {
            actor->set_folder_path(parent);
        } else {
            // A nested folder's actors keep their sub-path, re-rooted at the parent.
            const std::string remainder = folder.substr(path.size() + 1);
            actor->set_folder_path(parent.empty() ? remainder : parent + "/" + remainder);
        }
    }

    std::set<std::string> remaining;
    for (const std::string& folder : outliner_folders) {
        if (is_within(folder, path)) {
            if (folder == path) continue;   // the folder itself is gone
            const std::string remainder = folder.substr(path.size() + 1);
            remaining.insert(parent.empty() ? remainder : parent + "/" + remainder);
        } else {
            remaining.insert(folder);
        }
    }
    outliner_folders = std::move(remaining);
    scene_dirty = true;
}

void Editor::draw_outliner_actor(Actor* actor) {
    ImGui::PushID(actor);

    // Renaming replaces the row with an input field rather than opening a modal:
    // the name is right there, and a dialog for a one-word edit is heavy.
    if (renaming_actor == actor) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        // Focus the field the frame the rename starts, so typing works immediately
        // without a click. rename_focus_pending is cleared once it has been taken.
        if (rename_focus_pending) {
            ImGui::SetKeyboardFocusHere();
            rename_focus_pending = false;
        }
        const bool committed = ImGui::InputText("##rename_actor", renaming_actor_buf,
                                                sizeof(renaming_actor_buf),
                                                ImGuiInputTextFlags_EnterReturnsTrue |
                                                ImGuiInputTextFlags_AutoSelectAll);
        // Commit on click-away as well as Enter. Losing an edit because you clicked
        // elsewhere is infuriating, and it is how most people finish typing.
        if (committed || ImGui::IsItemDeactivatedAfterEdit()) {
            std::string wanted(renaming_actor_buf);
            // Trim: a name of only spaces produces an unreadable, unclickable row.
            const size_t first = wanted.find_first_not_of(" \t");
            if (first != std::string::npos) {
                wanted = wanted.substr(first, wanted.find_last_not_of(" \t") - first + 1);
                if (!wanted.empty() && wanted != actor->get_name()) {
                    // Uniqueness is enforced against every other actor, so two rows
                    // in the Outliner can never read identically.
                    actor->set_name(unique_name(wanted, [this, actor](const std::string& candidate) {
                        if (!active_actors) return false;
                        for (const auto& other : *active_actors) {
                            if (other && other.get() != actor && other->get_name() == candidate) {
                                return true;
                            }
                        }
                        return false;
                    }));
                    scene_dirty = true;
                }
            }
            renaming_actor = nullptr;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            renaming_actor = nullptr;
        }
        ImGui::PopID();
        return;
    }

    const std::string label = actor->get_name() + " (" + actor->shape_type + ")";
    const bool selected = is_actor_selected(actor);
    if (ImGui::Selectable(label.c_str(), selected)) {
        if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) {
            clear_selection();
        }
        if (selected && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift)) {
            for (auto it = selected_actors.begin(); it != selected_actors.end(); ++it) {
                if (*it == actor) { selected_actors.erase(it); break; }
            }
        } else if (!selected) {
            select_actor(actor);
        }
    }

    // Drag source. The payload is the actor pointer; the scene owns the actors and
    // outlives the drag, so a raw pointer is safe for the duration.
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        Actor* payload = actor;
        ImGui::SetDragDropPayload(kActorDragType, &payload, sizeof(Actor*));
        const size_t moving = is_actor_selected(actor) ? selected_actors.size() : 1;
        if (moving > 1) {
            ImGui::Text("Move %zu actors", moving);
        } else {
            ImGui::Text("Move %s", actor->get_name().c_str());
        }
        ImGui::EndDragDropSource();
    }

    // Double-click a selected row to rename, the way a file manager does. Guarded on
    // the row being hovered so a double-click in empty space does nothing.
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        renaming_actor = actor;
        rename_focus_pending = true;
        std::snprintf(renaming_actor_buf, sizeof(renaming_actor_buf), "%s",
                      actor->get_name().c_str());
    }

    if (ImGui::BeginPopupContextItem()) {
        if (!is_actor_selected(actor)) {
            clear_selection();
            select_actor(actor);
        }
        ImGui::TextDisabled("%s", actor->get_name().c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Rename", "F2")) {
            renaming_actor = actor;
            rename_focus_pending = true;
            std::snprintf(renaming_actor_buf, sizeof(renaming_actor_buf), "%s",
                          actor->get_name().c_str());
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D") && active_actors) {
            pending_duplicate = true;
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C")) {
            copy_selected_actors();
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !actor_clipboard.empty())) {
            pending_paste = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Create Prefab")) {
            create_prefab_from_actor(actor);
        }
        // Moving without dragging, which matters for a folder scrolled off screen.
        if (ImGui::BeginMenu("Move to Folder")) {
            if (ImGui::MenuItem("(root)")) {
                for (Actor* a : selected_actors) if (a) a->set_folder_path("");
                scene_dirty = true;
            }
            for (const std::string& folder : outliner_folders) {
                if (ImGui::MenuItem(folder.c_str())) {
                    for (Actor* a : selected_actors) if (a) a->set_folder_path(folder);
                    scene_dirty = true;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void Editor::draw_outliner_folder(const std::string& path, const std::string& display_name,
                                  std::vector<std::shared_ptr<Actor>>& actors) {
    // Immediate children of this folder, and the actors filed directly in it.
    std::vector<std::string> child_folders;
    for (const std::string& folder : outliner_folders) {
        if (folder_parent(folder) == path && !folder.empty()) child_folders.push_back(folder);
    }

    std::vector<Actor*> contained;
    for (const auto& actor : actors) {
        if (actor && actor->get_folder_path() == path && actor_matches_filter(actor.get())) {
            contained.push_back(actor.get());
        }
    }

    // A search hides folders that contain nothing matching, so filtering a large
    // scene does not leave a screen of empty folder rows to read past.
    if (outliner_filter[0] != '\0' && contained.empty()) {
        bool descendant_matches = false;
        for (const auto& actor : actors) {
            if (actor && is_within(actor->get_folder_path(), path) &&
                actor_matches_filter(actor.get())) {
                descendant_matches = true;
                break;
            }
        }
        if (!descendant_matches) return;
    }

    ImGui::PushID(path.c_str());

    // Renaming replaces the row with an input field, so the tree does not reflow
    // under a modal while the user is typing.
    if (renaming_folder == path) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool committed = ImGui::InputText("##rename_folder", renaming_folder_buf,
                                                sizeof(renaming_folder_buf),
                                                ImGuiInputTextFlags_EnterReturnsTrue |
                                                ImGuiInputTextFlags_AutoSelectAll);
        // Committing on lost focus as well as on Enter: clicking away is how most
        // people finish typing, and losing the edit there is infuriating.
        if (committed || (!ImGui::IsItemActive() && ImGui::IsItemDeactivated())) {
            rename_outliner_folder(path, renaming_folder_buf, actors);
            renaming_folder.clear();
        }
        ImGui::PopID();
        return;
    }

    // Auto-expand while searching, or a match nested three folders down is invisible.
    if (outliner_filter[0] != '\0') ImGui::SetNextItemOpen(true, ImGuiCond_Always);

    const std::string label = "[F] " + display_name;
    const bool open = ImGui::TreeNodeEx(label.c_str(),
                                        ImGuiTreeNodeFlags_SpanAvailWidth |
                                        ImGuiTreeNodeFlags_OpenOnArrow |
                                        ImGuiTreeNodeFlags_DefaultOpen);

    // Dropping onto the folder row files the dragged actors here.
    accept_actor_drop(path);

    if (ImGui::BeginPopupContextItem()) {
        ImGui::TextDisabled("%s", path.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("New Subfolder")) {
            pending_folder_parent = path;
            new_folder_name[0] = '\0';
            open_new_folder_popup = true;
        }
        if (ImGui::MenuItem("Rename")) {
            renaming_folder = path;
            std::snprintf(renaming_folder_buf, sizeof(renaming_folder_buf), "%s",
                          folder_leaf(path).c_str());
        }
        if (ImGui::MenuItem("Select Contents")) {
            clear_selection();
            for (const auto& actor : actors) {
                if (actor && is_within(actor->get_folder_path(), path)) select_actor(actor.get());
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete Folder")) {
            // Deferred: mutating the folder set while the tree that is iterating it
            // is still on the stack would invalidate the iteration.
            pending_folder_delete = path;
        }
        ImGui::TextDisabled("Contents move to the parent folder.");
        ImGui::EndPopup();
    }

    if (open) {
        std::sort(child_folders.begin(), child_folders.end());
        for (const std::string& child : child_folders) {
            draw_outliner_folder(child, folder_leaf(child), actors);
        }
        for (Actor* actor : contained) {
            draw_outliner_actor(actor);
        }
        if (child_folders.empty() && contained.empty()) {
            ImGui::TextDisabled("  (empty)");
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}


// =============================================================================
//  Duplicate, copy and paste
// =============================================================================



std::string Editor::next_available_name(const std::string& desired,
                                        const std::function<bool(const std::string&)>& taken) {
    return unique_name(desired, taken);
}

void Editor::duplicate_selected_actors(std::vector<std::shared_ptr<Actor>>& actors) {
    if (selected_actors.empty()) return;

    // The originals are captured before anything is appended, or the loop would
    // walk into the copies it is making and duplicate forever.
    const std::vector<Actor*> sources = selected_actors;
    std::vector<Actor*> created;

    for (Actor* source : sources) {
        if (!source) continue;
        std::shared_ptr<Actor> copy = SceneSerializer::clone_actor(source);
        if (!copy) continue;

        copy->set_name(unique_name(source->get_name(), [&actors](const std::string& candidate) {
            for (const auto& actor : actors) {
                if (actor && actor->get_name() == candidate) return true;
            }
            return false;
        }));
        // A duplicate stays in the folder its original was filed in, which is where
        // anyone organising a scene expects to find it.
        copy->set_folder_path(source->get_folder_path());
        created.push_back(copy.get());
        actors.push_back(std::move(copy));
    }

    // Select what was just made, not what it came from: the next thing anyone does
    // to a duplicate is move it.
    clear_selection();
    for (Actor* actor : created) select_actor(actor);
    scene_dirty = true;
}

void Editor::copy_selected_actors() {
    actor_clipboard.clear();
    for (Actor* actor : selected_actors) {
        if (!actor) continue;
        // Cloned into the clipboard rather than referenced, so a copy survives the
        // original being deleted before the paste.
        if (auto copy = SceneSerializer::clone_actor(actor)) {
            actor_clipboard.push_back(std::move(copy));
        }
    }
}

void Editor::paste_actors(std::vector<std::shared_ptr<Actor>>& actors) {
    if (actor_clipboard.empty()) return;
    std::vector<Actor*> created;

    for (const auto& stored : actor_clipboard) {
        // Cloned again on the way out, so pasting twice produces two independent
        // actors rather than two references to the clipboard's own copy.
        std::shared_ptr<Actor> copy = SceneSerializer::clone_actor(stored.get());
        if (!copy) continue;

        copy->set_name(unique_name(stored->get_name(), [&actors](const std::string& candidate) {
            for (const auto& actor : actors) {
                if (actor && actor->get_name() == candidate) return true;
            }
            return false;
        }));
        created.push_back(copy.get());
        actors.push_back(std::move(copy));
    }

    clear_selection();
    for (Actor* actor : created) select_actor(actor);
    scene_dirty = true;
}

void Editor::draw_outliner(std::vector<std::shared_ptr<Actor>>& actors) {
    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Scene Outliner");
    ImGui::Separator();

    if (ImGui::SmallButton("New Folder")) {
        pending_folder_parent.clear();   // top level
        new_folder_name[0] = '\0';
        open_new_folder_popup = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(drag actors onto a folder)");
    ImGui::Dummy(ImVec2(0.0f, 3.0f));

    // Name filter. A populated scene runs well past what fits in the panel, and the
    // list had no way to narrow it.
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##OutlinerFilter", "Search actors...", outliner_filter, sizeof(outliner_filter));
    ImGui::Dummy(ImVec2(0.0f, 3.0f));

    if (open_new_folder_popup) {
        ImGui::OpenPopup("New Folder");
        open_new_folder_popup = false;
    }
    if (ImGui::BeginPopupModal("New Folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (pending_folder_parent.empty()) {
            ImGui::TextDisabled("At the top level");
        } else {
            ImGui::TextDisabled("Inside %s", pending_folder_parent.c_str());
        }
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool submitted = ImGui::InputText("Name", new_folder_name, sizeof(new_folder_name),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        const bool confirmed = ImGui::Button("Create", ImVec2(120.0f, 0.0f)) || submitted;
        ImGui::SameLine();
        const bool cancelled = ImGui::Button("Cancel", ImVec2(120.0f, 0.0f));

        if (confirmed) {
            const std::string cleaned = sanitize_folder_name(new_folder_name);
            if (!cleaned.empty()) {
                const std::string path = pending_folder_parent.empty()
                    ? cleaned
                    : pending_folder_parent + "/" + cleaned;
                outliner_folders.insert(path);
                scene_dirty = true;
            }
            ImGui::CloseCurrentPopup();
        }
        if (cancelled) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (actors.empty() && outliner_folders.empty()) {
        ImGui::TextDisabled("No Actors in World");
        return;
    }

    // A folder path on an actor is authoritative: a scene loaded from a file, or one
    // whose actor was moved by a script, can reference a folder this set has never
    // seen. Adopting it here means the actor is never orphaned into invisibility.
    for (const auto& actor : actors) {
        if (!actor || actor->get_folder_path().empty()) continue;
        std::string path = actor->get_folder_path();
        // Every ancestor has to exist too, or the tree has a hole in the middle and
        // the leaf never gets drawn.
        while (!path.empty()) {
            outliner_folders.insert(path);
            const size_t slash = path.find_last_of('/');
            path = (slash == std::string::npos) ? std::string() : path.substr(0, slash);
        }
    }

    // Top-level folders first, then the actors filed at the root.
    std::vector<std::string> roots;
    for (const std::string& folder : outliner_folders) {
        if (folder.find('/') == std::string::npos) roots.push_back(folder);
    }
    std::sort(roots.begin(), roots.end());

    ImGui::BeginChild("OutlinerTree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    for (const std::string& folder : roots) {
        draw_outliner_folder(folder, folder, actors);
    }

    int shown = 0;
    for (const auto& actor : actors) {
        if (!actor || !actor->get_folder_path().empty()) continue;
        if (!actor_matches_filter(actor.get())) continue;
        draw_outliner_actor(actor.get());
        ++shown;
    }

    // Dropping on empty space below the tree files actors back at the root, which is
    // the only way to get one out of a folder by dragging.
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(24.0f, ImGui::GetContentRegionAvail().y)));
    accept_actor_drop("");
    ImGui::EndChild();

    if (shown == 0 && roots.empty() && outliner_filter[0] != '\0') {
        ImGui::TextDisabled("No actors match \"%s\"", outliner_filter);
    }

    // Applied after the tree has been walked, so neither the folder set nor the
    // actor list is mutated while the recursion that reads them is still live.
    if (!pending_folder_delete.empty()) {
        delete_outliner_folder(pending_folder_delete, actors);
        pending_folder_delete.clear();
    }

    // Shortcuts, handled while the Outliner has focus so they cannot fire while the
    // user is typing into a field elsewhere in the editor. WantTextInput covers the
    // rename field itself - Ctrl+V there belongs to the text box, not to us.
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) pending_duplicate = true;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) copy_selected_actors();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) pending_paste = true;
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && selected_actors.size() == 1) {
            renaming_actor = selected_actors[0];
            rename_focus_pending = true;
            std::snprintf(renaming_actor_buf, sizeof(renaming_actor_buf), "%s",
                          selected_actors[0]->get_name().c_str());
        }
    }

    if (pending_duplicate) {
        duplicate_selected_actors(actors);
        pending_duplicate = false;
    }
    if (pending_paste) {
        paste_actors(actors);
        pending_paste = false;
    }
}


// --- Game UI authoring -----------------------------------------------------

namespace {

// Anchor presets. Nine corner/edge positions plus a stretch, which is the whole
// vocabulary anchoring actually needs: everything else is those with different
// offsets. y grows downward, so "Top" is anchor 0 and "Bottom" is anchor 1.
struct AnchorPreset {
    const char* label;
    float min_x, min_y, max_x, max_y;
};

const AnchorPreset kAnchorPresets[] = {
    { "Top Left",      0.0f, 0.0f, 0.0f, 0.0f },
    { "Top Center",    0.5f, 0.0f, 0.5f, 0.0f },
    { "Top Right",     1.0f, 0.0f, 1.0f, 0.0f },
    { "Middle Left",   0.0f, 0.5f, 0.0f, 0.5f },
    { "Center",        0.5f, 0.5f, 0.5f, 0.5f },
    { "Middle Right",  1.0f, 0.5f, 1.0f, 0.5f },
    { "Bottom Left",   0.0f, 1.0f, 0.0f, 1.0f },
    { "Bottom Center", 0.5f, 1.0f, 0.5f, 1.0f },
    { "Bottom Right",  1.0f, 1.0f, 1.0f, 1.0f },
    { "Stretch All",   0.0f, 0.0f, 1.0f, 1.0f },
    { "Stretch Top",   0.0f, 0.0f, 1.0f, 0.0f },
    { "Stretch Bottom",0.0f, 1.0f, 1.0f, 1.0f },
};
const int kAnchorPresetCount = (int)(sizeof(kAnchorPresets) / sizeof(kAnchorPresets[0]));

// Re-anchors a widget and rewrites its offsets so it keeps its current size and
// sits sensibly against the new anchor. Offsets are signed away from the anchored
// edge, which is what makes a bottom-right widget move with that corner instead of
// sliding off screen when the window is resized.
void apply_anchor_preset(UIWidget& w, const AnchorPreset& preset, float margin) {
    const float width  = std::abs(w.offset_max.x - w.offset_min.x);
    const float height = std::abs(w.offset_max.y - w.offset_min.y);

    w.anchor_min = { preset.min_x, preset.min_y };
    w.anchor_max = { preset.max_x, preset.max_y };

    const bool stretch_x = (preset.min_x != preset.max_x);
    const bool stretch_y = (preset.min_y != preset.max_y);

    if (stretch_x) {
        w.offset_min.x = margin;
        w.offset_max.x = -margin;
    } else if (preset.min_x >= 1.0f) {
        w.offset_min.x = -width - margin;
        w.offset_max.x = -margin;
    } else if (preset.min_x > 0.0f) {
        w.offset_min.x = -width * 0.5f;
        w.offset_max.x = width * 0.5f;
    } else {
        w.offset_min.x = margin;
        w.offset_max.x = margin + width;
    }

    if (stretch_y) {
        w.offset_min.y = margin;
        w.offset_max.y = -margin;
    } else if (preset.min_y >= 1.0f) {
        w.offset_min.y = -height - margin;
        w.offset_max.y = -margin;
    } else if (preset.min_y > 0.0f) {
        w.offset_min.y = -height * 0.5f;
        w.offset_max.y = height * 0.5f;
    } else {
        w.offset_min.y = margin;
        w.offset_max.y = margin + height;
    }
}

bool color_edit(const char* label, Vector4& color) {
    float c[4] = { color.x, color.y, color.z, color.w };
    if (ImGui::ColorEdit4(label, c, ImGuiColorEditFlags_AlphaBar)) {
        color = { c[0], c[1], c[2], c[3] };
        return true;
    }
    return false;
}

} // namespace

void Editor::draw_ui_widget_tree(UICanvasComponent* canvas, UIWidget* widget) {
    if (!widget) return;

    ImGui::PushID(widget);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (widget->children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (widget == selected_widget) flags |= ImGuiTreeNodeFlags_Selected;

    const bool open = ImGui::TreeNodeEx("##node", flags, "%s  (%s)%s",
                                        widget->name.c_str(),
                                        UIWidget::type_name(widget->type),
                                        widget->visible ? "" : "  [hidden]");
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selected_widget = widget;
    }

    if (open && !widget->children.empty()) {
        for (auto& child : widget->children) {
            draw_ui_widget_tree(canvas, child.get());
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void Editor::draw_ui_widget_properties(UICanvasComponent* canvas, UIWidget* widget) {
    if (!canvas || !widget) return;

    char name_buf[128];
    strncpy(name_buf, widget->name.c_str(), sizeof(name_buf));
    name_buf[sizeof(name_buf) - 1] = '\0';
    if (ImGui::InputText("Name", name_buf, sizeof(name_buf))) {
        // Scripts address widgets by name, so a duplicate would make one of them
        // permanently unreachable. Silently uniquifying beats accepting it.
        std::string desired(name_buf);
        if (desired.empty()) desired = UIWidget::type_name(widget->type);
        widget->name = canvas->name_in_use(desired, widget) ? canvas->make_unique_name(desired) : desired;
    }

    const char* widget_types[UIWidget::Widget_Count];
    for (int i = 0; i < UIWidget::Widget_Count; ++i) widget_types[i] = UIWidget::type_name(i);
    ImGui::Combo("Type", &widget->type, widget_types, UIWidget::Widget_Count);

    ImGui::Checkbox("Visible", &widget->visible);
    ImGui::SameLine();
    ImGui::Checkbox("Interactive", &widget->interactive);
    if (!UIWidget::type_is_interactive(widget->type) && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("For a non-interactive kind this only controls whether the\n"
                          "widget blocks clicks from reaching what is behind it.");
    }

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::TextDisabled("Rect");
    static int preset_index = 4;
    if (ImGui::BeginCombo("Anchor Preset", kAnchorPresets[preset_index].label)) {
        for (int i = 0; i < kAnchorPresetCount; ++i) {
            if (ImGui::Selectable(kAnchorPresets[i].label, i == preset_index)) {
                preset_index = i;
                apply_anchor_preset(*widget, kAnchorPresets[i], 16.0f);
            }
        }
        ImGui::EndCombo();
    }

    float amin[2] = { widget->anchor_min.x, widget->anchor_min.y };
    if (ImGui::DragFloat2("Anchor Min", amin, 0.01f, 0.0f, 1.0f)) {
        widget->anchor_min = { amin[0], amin[1] };
    }
    float amax[2] = { widget->anchor_max.x, widget->anchor_max.y };
    if (ImGui::DragFloat2("Anchor Max", amax, 0.01f, 0.0f, 1.0f)) {
        widget->anchor_max = { amax[0], amax[1] };
    }
    float omin[2] = { widget->offset_min.x, widget->offset_min.y };
    if (ImGui::DragFloat2("Offset Min", omin, 1.0f)) {
        widget->offset_min = { omin[0], omin[1] };
    }
    float omax[2] = { widget->offset_max.x, widget->offset_max.y };
    if (ImGui::DragFloat2("Offset Max", omax, 1.0f)) {
        widget->offset_max = { omax[0], omax[1] };
    }
    ImGui::TextDisabled("Resolved: %.0f, %.0f  %.0f x %.0f",
                        widget->computed_rect.x, widget->computed_rect.y,
                        widget->computed_rect.width, widget->computed_rect.height);

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::TextDisabled("Appearance");
    color_edit("Background", widget->background_color);
    color_edit("Border", widget->border_color);
    ImGui::DragFloat("Border Thickness", &widget->border_thickness, 0.1f, 0.0f, 20.0f);
    ImGui::DragFloat("Corner Radius", &widget->corner_radius, 0.5f, 0.0f, 64.0f);
    ImGui::DragFloat("Padding", &widget->padding, 0.5f, 0.0f, 64.0f);

    if (UIWidget::type_has_text(widget->type)) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextDisabled("Text");
        char text_buf[512];
        strncpy(text_buf, widget->text.c_str(), sizeof(text_buf));
        text_buf[sizeof(text_buf) - 1] = '\0';
        if (ImGui::InputTextMultiline("##text", text_buf, sizeof(text_buf), ImVec2(-FLT_MIN, 54.0f))) {
            widget->text = text_buf;
        }
        color_edit("Text Color", widget->text_color);
        ImGui::DragFloat("Font Scale", &widget->font_scale, 0.02f, 0.2f, 8.0f);
        const char* aligns[] = { "Start", "Center", "End" };
        ImGui::Combo("H Align", &widget->h_align, aligns, 3);
        ImGui::Combo("V Align", &widget->v_align, aligns, 3);
        ImGui::Checkbox("Word Wrap", &widget->word_wrap);
    }

    if (widget->type == UIWidget::Widget_Image) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextDisabled("Image");
        char image_buf[256];
        strncpy(image_buf, widget->image_path.c_str(), sizeof(image_buf));
        image_buf[sizeof(image_buf) - 1] = '\0';
        if (ImGui::InputText("Texture", image_buf, sizeof(image_buf))) {
            widget->image_path = image_buf;
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_FILE")) {
                widget->image_path = std::string((const char*)payload->Data);
            }
            ImGui::EndDragDropTarget();
        }
        color_edit("Tint", widget->image_tint);
    }

    if (UIWidget::type_is_interactive(widget->type)) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextDisabled("Interaction colors");
        color_edit("Hover", widget->hover_color);
        color_edit("Pressed", widget->pressed_color);
        color_edit("Disabled", widget->disabled_color);
    }

    if (UIWidget::type_has_value(widget->type)) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextDisabled("Value");
        ImGui::DragFloat("Value", &widget->value, 0.01f);
        ImGui::DragFloat("Min", &widget->min_value, 0.01f);
        ImGui::DragFloat("Max", &widget->max_value, 0.01f);
        color_edit("Fill", widget->fill_color);
    }
}

void Editor::draw_ui_canvas_panel(UICanvasComponent* canvas) {
    if (!canvas) return;

    // The selection is a raw pointer into a tree the panel itself edits, so it has
    // to be proved to still belong to this canvas before anything dereferences it.
    // Checked by pointer rather than by name: a deleted widget's name cannot be
    // read back to look it up with.
    if (!canvas->contains(selected_widget)) selected_widget = nullptr;

    float ref[2] = { canvas->reference_resolution.x, canvas->reference_resolution.y };
    if (ImGui::DragFloat2("Reference Resolution", ref, 1.0f, 64.0f, 8192.0f)) {
        canvas->reference_resolution = { ref[0], ref[1] };
    }
    const char* modes[] = { "Constant Pixel Size", "Scale With Screen Size" };
    ImGui::Combo("Scale Mode", &canvas->scale_mode, modes, 2);
    if (canvas->scale_mode == UICanvasComponent::Scale_WithScreenSize) {
        ImGui::SliderFloat("Match W/H", &canvas->match_width_or_height, 0.0f, 1.0f, "%.2f");
    }
    ImGui::DragInt("Sort Order", &canvas->sort_order, 0.1f, -64, 64);
    ImGui::Checkbox("Visible", &canvas->visible);
    ImGui::SameLine();
    ImGui::Checkbox("Preview In Editor", &canvas->show_in_editor);
    ImGui::TextDisabled("Widgets only take input while the game is playing.");

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    if (ImGui::Button("Add Widget...")) ImGui::OpenPopup("AddUIWidget");
    if (ImGui::BeginPopup("AddUIWidget")) {
        // Added under the selection when there is one, so building a dialog is a
        // matter of selecting the panel and adding its contents.
        UIWidget* parent = selected_widget;
        if (parent) ImGui::TextDisabled("Inside '%s'", parent->name.c_str());
        else        ImGui::TextDisabled("At the canvas root");
        ImGui::Separator();
        for (int i = 0; i < UIWidget::Widget_Count; ++i) {
            if (ImGui::MenuItem(UIWidget::type_name(i))) {
                selected_widget = canvas->add_widget(i, parent, UIWidget::type_name(i));
            }
        }
        ImGui::EndPopup();
    }
    if (selected_widget) {
        ImGui::SameLine();
        if (ImGui::Button("Delete Widget")) {
            canvas->remove_widget(selected_widget);
            selected_widget = nullptr;
        }
    }

    if (canvas->roots.empty()) {
        ImGui::TextDisabled("No widgets yet. Add one to start the layout.");
        return;
    }

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    if (ImGui::BeginChild("UIWidgetTree", ImVec2(0.0f, 130.0f), true)) {
        for (auto& widget : canvas->roots) {
            draw_ui_widget_tree(canvas, widget.get());
        }
    }
    ImGui::EndChild();

    if (selected_widget) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::Separator();
        ImGui::PushID(selected_widget);
        draw_ui_widget_properties(canvas, selected_widget);
        ImGui::PopID();
    } else {
        ImGui::TextDisabled("Select a widget to edit it.");
    }
}


// --- Terrain ---------------------------------------------------------------

void Editor::apply_terrain_brush(std::vector<std::shared_ptr<Actor>>& actors,
                                 const Matrix4x4& view, float ndc_x, float ndc_y, float aspect) {
    if (terrain_brush == TerrainBrush_None) return;

    // Only the selected actor's terrain is painted. Anything else and a brush stroke
    // near a boundary would silently edit a terrain the user was not looking at.
    TerrainComponent* terrain = nullptr;
    for (Actor* actor : selected_actors) {
        if (!actor) continue;
        if (auto* found = actor->get_component<TerrainComponent>()) { terrain = found; break; }
    }
    if (!terrain) return;

    // Same ray construction the viewport's actor picking uses: the view matrix is
    // rotation-only under large-world-coordinates, so its columns are the camera
    // basis and the origin is the camera position itself.
    const Vector3 right   = { view.m[0], view.m[4], view.m[8] };
    const Vector3 up      = { view.m[1], view.m[5], view.m[9] };
    const Vector3 forward = { -view.m[2], -view.m[6], -view.m[10] };

    const float fov_radians = 45.0f * (3.14159265f / 180.0f);
    const float tan_half_fov = std::tan(fov_radians * 0.5f);
    Vector3 direction = right * (ndc_x * aspect * tan_half_fov) +
                        up * (ndc_y * tan_half_fov) +
                        forward;
    direction = direction.normalized();

    const DVector3 origin = active_renderer ? active_renderer->get_camera_pos()
                                            : (g_engine ? g_engine->get_camera_position()
                                                        : DVector3{ 0.0, 0.0, 0.0 });

    DVector3 hit;
    if (!terrain->raycast(origin, direction, 5000.0f, hit)) return;

    // The brush works in the terrain's local space, where the surface is an
    // axis-aligned height field.
    const DVector3 local = hit - terrain->placement().position;
    const float local_x = static_cast<float>(local.x);
    const float local_z = static_cast<float>(local.z);

    // Frame-rate independent: a stroke should deposit the same material whether the
    // editor is running at 30 or 200 frames a second.
    const float delta = ImGui::GetIO().DeltaTime;

    switch (terrain_brush) {
        case TerrainBrush_Raise:
        case TerrainBrush_Lower:
        case TerrainBrush_Smooth:
            terrain->sculpt(terrain_brush, local_x, local_z, terrain_brush_radius,
                            terrain_brush_strength * delta, terrain_flatten_height);
            break;
        case TerrainBrush_Flatten:
            terrain->sculpt(TerrainComponent::Sculpt_Flatten, local_x, local_z,
                            terrain_brush_radius, terrain_brush_strength * delta,
                            terrain_flatten_height);
            break;
        case TerrainBrush_Paint:
            terrain->paint_layer(terrain_paint_layer, local_x, local_z, terrain_brush_radius,
                                 terrain_brush_strength * delta);
            break;
        case TerrainBrush_Foliage:
            terrain->paint_foliage(local_x, local_z, terrain_brush_radius,
                                   terrain_brush_strength * delta, terrain_foliage_erase);
            break;
        default:
            break;
    }
    mark_scene_dirty();
}

void Editor::draw_terrain_panel(TerrainComponent* terrain) {
    if (!terrain) return;

    int resolution = terrain->get_resolution();
    float world_size = terrain->get_world_size();
    bool shape_changed = false;
    if (ImGui::DragInt("Resolution", &resolution, 1.0f,
                       TerrainComponent::kMinResolution, TerrainComponent::kMaxResolution)) {
        shape_changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Height samples per side. Rounded down to a power of two.\n"
                          "Existing sculpting is resampled, not discarded.");
    }
    if (ImGui::DragFloat("Size", &world_size, 1.0f, 4.0f, 8192.0f, "%.0f m")) shape_changed = true;
    if (shape_changed) {
        terrain->resize(resolution, world_size);
        mark_scene_dirty();
    }
    ImGui::TextDisabled("%d x %d samples over %.0f m (%.2f m per cell)",
                        terrain->get_resolution(), terrain->get_resolution(),
                        terrain->get_world_size(),
                        terrain->get_world_size() / std::max(1, terrain->get_resolution() - 1));

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextDisabled("Brush");
    const char* brush_names[] = { "None", "Raise", "Lower", "Smooth", "Flatten", "Paint Layer", "Foliage" };
    int brush_index = terrain_brush + 1;
    if (ImGui::Combo("Tool", &brush_index, brush_names, 7)) {
        terrain_brush = brush_index - 1;
    }
    if (terrain_brush != TerrainBrush_None) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.45f, 1.0f),
                           "Drag in the viewport to apply. The gizmo is off\n"
                           "while a brush is selected.");
        ImGui::DragFloat("Radius", &terrain_brush_radius, 0.2f, 0.5f, 200.0f, "%.1f m");
        ImGui::DragFloat("Strength", &terrain_brush_strength, 0.1f, 0.05f, 100.0f, "%.2f");
        if (terrain_brush == TerrainBrush_Flatten) {
            ImGui::DragFloat("Target Height", &terrain_flatten_height, 0.1f, -1000.0f, 1000.0f, "%.2f m");
        }
        if (terrain_brush == TerrainBrush_Paint) {
            ImGui::SliderInt("Layer", &terrain_paint_layer, 0, TerrainComponent::kLayerCount - 1);
        }
        if (terrain_brush == TerrainBrush_Foliage) {
            ImGui::Checkbox("Erase", &terrain_foliage_erase);
        }
    }
    if (ImGui::Button("Flatten Everything")) {
        terrain->reset();
        mark_scene_dirty();
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextDisabled("Layers");
    ImGui::TextDisabled("A layer with no texture draws a flat colour, so an");
    ImGui::TextDisabled("unpainted terrain is still readable.");
    for (int layer = 0; layer < TerrainComponent::kLayerCount; ++layer) {
        ImGui::PushID(layer);
        char path_buf[256];
        strncpy(path_buf, terrain->layer_texture_path[layer].c_str(), sizeof(path_buf));
        path_buf[sizeof(path_buf) - 1] = '\0';
        char label[32];
        snprintf(label, sizeof(label), "Layer %d", layer);
        if (ImGui::InputText(label, path_buf, sizeof(path_buf))) {
            terrain->layer_texture_path[layer] = path_buf;
            mark_scene_dirty();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_FILE")) {
                terrain->layer_texture_path[layer] = std::string((const char*)payload->Data);
                mark_scene_dirty();
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::DragFloat("Tiling", &terrain->layer_tiling[layer], 0.5f, 0.5f, 512.0f, "%.0f");
        ImGui::PopID();
    }
    draw_layer_combo("Collision Layer##terrain", terrain->collision_layer);
    ImGui::DragFloat("Metallic##terrain", &terrain->metallic, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Roughness##terrain", &terrain->roughness, 0.01f, 0.0f, 1.0f);

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextDisabled("Foliage");
    char foliage_buf[256];
    strncpy(foliage_buf, terrain->foliage_mesh_path.c_str(), sizeof(foliage_buf));
    foliage_buf[sizeof(foliage_buf) - 1] = '\0';
    if (ImGui::InputText("Mesh", foliage_buf, sizeof(foliage_buf))) {
        terrain->foliage_mesh_path = foliage_buf;
        mark_scene_dirty();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_FILE")) {
            terrain->foliage_mesh_path = std::string((const char*)payload->Data);
            mark_scene_dirty();
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::DragFloat("Density", &terrain->foliage_density, 0.05f, 0.0f, 20.0f, "%.2f per m2");
    ImGui::DragFloat("Min Scale", &terrain->foliage_min_scale, 0.01f, 0.05f, 10.0f);
    ImGui::DragFloat("Max Scale", &terrain->foliage_max_scale, 0.01f, 0.05f, 10.0f);
    ImGui::DragFloat("Max Slope##foliage", &terrain->foliage_max_slope_degrees, 1.0f, 0.0f, 89.0f, "%.0f deg");
    ImGui::DragInt("Seed", &terrain->foliage_seed, 1.0f, 0, 100000);
    ImGui::DragInt("Max Instances", &terrain->foliage_max_instances, 100.0f, 0, 500000);
    ImGui::TextDisabled("%zu instances scattered", terrain->get_foliage_instances().size());

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextDisabled("Heightmap file");
    char data_buf[256];
    strncpy(data_buf, terrain->data_path.c_str(), sizeof(data_buf));
    data_buf[sizeof(data_buf) - 1] = '\0';
    if (ImGui::InputText("Path", data_buf, sizeof(data_buf))) terrain->data_path = data_buf;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Heights, splat weights and foliage coverage are written here\n"
                          "when the scene is saved. Megabytes of numbers do not belong\n"
                          "inside the scene file.");
    }
}

void Editor::draw_properties() {
    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Details / Properties");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 5.0f));

    if (selected_actors.empty()) {
        ImGui::TextDisabled("Select an Actor in the Outliner\nto view properties.");
        return;
    }
    
    if (selected_actors.size() > 1) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "%zu Objects Selected", selected_actors.size());
        ImGui::Separator();
    }

    Actor* primary_actor = selected_actors[0];

    // Prefab linkage. Shown first because it changes what every field below means:
    // on a linked instance, an edited field stops following the prefab.
    if (!primary_actor->prefab_source.empty()) {
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "Prefab");
        ImGui::TextDisabled("%s", primary_actor->prefab_source.c_str());

        const std::vector<std::string> overrides =
            SceneSerializer::list_prefab_overrides(primary_actor);
        if (overrides.empty()) {
            ImGui::TextDisabled("Matches the prefab exactly.");
        } else {
            std::string summary;
            for (const std::string& name : overrides) {
                if (!summary.empty()) summary += ", ";
                summary += name;
            }
            ImGui::TextWrapped("Overridden: %s", summary.c_str());
        }

        if (ImGui::Button("Apply To Prefab")) {
            // Pushes this instance's state back onto the asset, so every other
            // instance that has not overridden the same thing picks it up.
            if (SceneSerializer::apply_actor_to_prefab(primary_actor->prefab_source, primary_actor)) {
                scene_dirty = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            // Rebuilt from the asset, then put back in the same slot so anything
            // holding this actor's place in the scene list is undisturbed. The name
            // is kept: scripts address actors by name, and reverting to the prefab's
            // name would silently break every reference to this instance.
            if (auto fresh = SceneSerializer::load_prefab(primary_actor->prefab_source)) {
                fresh->prefab_source = primary_actor->prefab_source;
                fresh->set_name(primary_actor->get_name());
                if (active_actors) {
                    for (auto& entry : *active_actors) {
                        if (entry.get() != primary_actor) continue;
                        entry = fresh;
                        clear_selection();
                        select_actor(fresh.get());
                        scene_dirty = true;
                        break;
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Unlink")) {
            // Breaks the connection and keeps the current state. The actor is then
            // written out in full rather than as a set of differences.
            primary_actor->prefab_source.clear();
            scene_dirty = true;
        }
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }

    // Static SLR volumetric beam properties.
    if (auto* slr = dynamic_cast<StaticSLRActor*>(primary_actor)) {
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "Static Light Ray (SLR)");
        ImGui::Separator();

        // A full-width swatch that opens the picker, rather than four cramped number
        // boxes: the Details panel is narrow and the numeric form was unreadable.
        float color[4] = { slr->slr_color.x, slr->slr_color.y, slr->slr_color.z, slr->slr_alpha };
        ImGuiColorEditFlags color_flags = ImGuiColorEditFlags_Float |
                                          ImGuiColorEditFlags_NoInputs |
                                          ImGuiColorEditFlags_AlphaBar |
                                          ImGuiColorEditFlags_AlphaPreviewHalf |
                                          ImGuiColorEditFlags_PickerHueWheel;
        ImGui::Text("Light Color");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::ColorEdit4("##slr_light_color", color, color_flags)) {
            slr->slr_color = { color[0], color[1], color[2] };
            slr->slr_alpha = color[3];
            scene_dirty = true;
        }
        ImGui::TextDisabled("Click the swatch to open the picker.");

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        static const char* slr_shapes[] = { "Box Beam", "Cone (Spotlight)" };
        if (ImGui::Combo("Shape", &slr->shape, slr_shapes, 2)) scene_dirty = true;

        if (ImGui::SliderFloat("Sharpness", &slr->sharpness, 0.0f, 1.0f, "%.3f")) {
            scene_dirty = true;
        }
        ImGui::TextDisabled("0 = soft, diffused edges   1 = crisp, hard corners");

        if (ImGui::SliderFloat("Intensity", &slr->intensity, 0.0f, 10.0f, "%.2f")) {
            scene_dirty = true;
        }
        // Falloff and Core are what separate a beam of light from a slab of fog:
        // how fast it dims along its length, and how tightly energy hugs its axis.
        if (ImGui::SliderFloat("Falloff", &slr->falloff, 0.0f, 6.0f, "%.2f")) {
            scene_dirty = true;
        }
        ImGui::TextDisabled("Dimming along the beam's length.");
        if (ImGui::SliderFloat("Core", &slr->core, 0.0f, 1.0f, "%.2f")) {
            scene_dirty = true;
        }
        ImGui::TextDisabled("0 = even across width   1 = tight bright core");

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }

    // Actor Name (only editable if one actor is selected)
    char name_buf[64];
    strncpy(name_buf, primary_actor->get_name().c_str(), sizeof(name_buf));
    if (selected_actors.size() == 1) {
        // Committed on Enter or on click-away, not per keystroke. Renaming on every
        // character would run the uniqueness check against half-typed names and
        // append a "(1)" the moment the partial name happened to match another actor.
        if (ImGui::InputText("Actor Name", name_buf, sizeof(name_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            std::string wanted(name_buf);
            const size_t first = wanted.find_first_not_of(" \t");
            if (first != std::string::npos) {
                wanted = wanted.substr(first, wanted.find_last_not_of(" \t") - first + 1);
            } else {
                wanted.clear();
            }
            if (!wanted.empty() && wanted != primary_actor->get_name()) {
                primary_actor->set_name(unique_name(wanted,
                    [this, primary_actor](const std::string& candidate) {
                        if (!active_actors) return false;
                        for (const auto& other : *active_actors) {
                            if (other && other.get() != primary_actor &&
                                other->get_name() == candidate) {
                                return true;
                            }
                        }
                        return false;
                    }));
                scene_dirty = true;
            }
            // Reflect whatever the name actually became, including any suffix.
            std::snprintf(name_buf, sizeof(name_buf), "%s", primary_actor->get_name().c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Duplicate")) pending_duplicate = true;
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    // --------------------------------------------------------
    // Transform
    // --------------------------------------------------------
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Transform");
    ImGui::Separator();
    
    bool any_activated = false;
    bool any_deactivated = false;

    // Position
    float pos[3] = { static_cast<float>(primary_actor->get_actor_transform().position.x), static_cast<float>(primary_actor->get_actor_transform().position.y), static_cast<float>(primary_actor->get_actor_transform().position.z) };
    if (ImGui::DragFloat3("Position", pos, 0.05f)) {
        DVector3 delta = { static_cast<double>(pos[0]) - primary_actor->get_actor_transform().position.x, static_cast<double>(pos[1]) - primary_actor->get_actor_transform().position.y, static_cast<double>(pos[2]) - primary_actor->get_actor_transform().position.z };
        for (Actor* actor : selected_actors) {
            actor->get_actor_transform().position += delta;
        }
    }
    if (ImGui::IsItemActivated()) any_activated = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) any_deactivated = true;

    // Rotation (Convert to degrees for UI)
    float rot[3] = { 
        primary_actor->get_actor_transform().rotation.x * 180.0f / 3.14159f, 
        primary_actor->get_actor_transform().rotation.y * 180.0f / 3.14159f, 
        primary_actor->get_actor_transform().rotation.z * 180.0f / 3.14159f 
    };
    if (ImGui::DragFloat3("Rotation", rot, 1.0f)) {
        Vector3 delta = { 
            (rot[0] * 3.14159f / 180.0f) - primary_actor->get_actor_transform().rotation.x, 
            (rot[1] * 3.14159f / 180.0f) - primary_actor->get_actor_transform().rotation.y, 
            (rot[2] * 3.14159f / 180.0f) - primary_actor->get_actor_transform().rotation.z 
        };
        for (Actor* actor : selected_actors) {
            actor->get_actor_transform().rotation += delta;
        }
    }
    if (ImGui::IsItemActivated()) any_activated = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) any_deactivated = true;

    // Scale
    float scl[3] = { primary_actor->get_actor_transform().scale.x, primary_actor->get_actor_transform().scale.y, primary_actor->get_actor_transform().scale.z };
    if (ImGui::DragFloat3("Scale", scl, 0.05f, 0.01f, 100.0f)) {
        Vector3 delta = { 
            primary_actor->get_actor_transform().scale.x != 0.0f ? scl[0] / primary_actor->get_actor_transform().scale.x : 1.0f, 
            primary_actor->get_actor_transform().scale.y != 0.0f ? scl[1] / primary_actor->get_actor_transform().scale.y : 1.0f, 
            primary_actor->get_actor_transform().scale.z != 0.0f ? scl[2] / primary_actor->get_actor_transform().scale.z : 1.0f 
        };
        for (Actor* actor : selected_actors) {
            actor->get_actor_transform().scale.x *= delta.x;
            actor->get_actor_transform().scale.y *= delta.y;
            actor->get_actor_transform().scale.z *= delta.z;
        }
    }
    if (ImGui::IsItemActivated()) any_activated = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) any_deactivated = true;

    if (any_activated && !is_dragging) {
        is_dragging = true;
        drag_start_states.clear();
        for (Actor* actor : selected_actors) {
            drag_start_states.push_back({actor, actor->get_actor_transform()});
        }
    }

    if (any_deactivated && is_dragging) {
        is_dragging = false;
        std::vector<TransformCommand::ActorTransformState> drag_end_states;
        for (Actor* actor : selected_actors) {
            drag_end_states.push_back({actor, actor->get_actor_transform()});
        }
        redo_stack.clear();
        undo_stack.push_back(std::make_unique<TransformCommand>(drag_start_states, drag_end_states));
        scene_dirty = true;
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Visuals");
    ImGui::Separator();

    // Visibility
    bool invisible = primary_actor->is_invisible;
    if (ImGui::Checkbox("Invisibility Mode", &invisible)) {
        for (Actor* actor : selected_actors) {
            actor->is_invisible = invisible;
        }
    }

    // Color Pick
    float col[3] = { primary_actor->actor_color.x, primary_actor->actor_color.y, primary_actor->actor_color.z };
    if (ImGui::ColorEdit3("Shade Color", col)) {
        for (Actor* actor : selected_actors) {
            actor->actor_color = { col[0], col[1], col[2] };
        }
    }
    
    // PBR Properties
    float met = primary_actor->metallic;
    if (ImGui::SliderFloat("Metallic", &met, 0.0f, 1.0f)) {
        for (Actor* actor : selected_actors) {
            actor->metallic = met;
        }
    }
    float rough = primary_actor->roughness;
    if (ImGui::SliderFloat("Roughness", &rough, 0.0f, 1.0f)) {
        for (Actor* actor : selected_actors) {
            actor->roughness = rough;
        }
    }
    bool is_static = primary_actor->is_static;
    if (ImGui::Checkbox("Static (bakeable)", &is_static)) {
        for (Actor* actor : selected_actors) actor->is_static = is_static;
        scene_dirty = true;
    }
    if (primary_actor->has_baked_lighting) {
        ImGui::TextDisabled("Baked irradiance: %.3f", primary_actor->baked_irradiance);
    }

    float emis = primary_actor->emissive;
    if (ImGui::SliderFloat("Emissive", &emis, 0.0f, 20.0f, "%.2f")) {
        for (Actor* actor : selected_actors) {
            actor->emissive = emis;
        }
        scene_dirty = true;
    }
    ImGui::TextDisabled("Self-illumination. Above ~1 it starts to bloom.");

    // Emission colour is separate from the intensity above: together they are what
    // makes a warm bulb in a grey housing expressible. Multiplying albedo by a
    // scalar, as this used to, meant a white object could only glow white and a
    // black one could not glow at all.
    float emission_col[3] = { primary_actor->emission_color.x,
                              primary_actor->emission_color.y,
                              primary_actor->emission_color.z };
    if (ImGui::ColorEdit3("Emission Color", emission_col)) {
        for (Actor* actor : selected_actors) {
            actor->emission_color = { emission_col[0], emission_col[1], emission_col[2] };
        }
        scene_dirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Hue of the glow. The Emissive slider above is its\n"
                          "brightness - a grey lamp with a warm bulb is the two\n"
                          "of these together.");
    }

    // --- Surface response ----------------------------------------------------
    //
    // These four already drive the lighting shader - the geometry pass has carried
    // them per draw for a long time - but there was no way to set them from the
    // editor, so every object in every scene shipped with them at their defaults.
    // Each writes to the whole selection, like the sliders above.
    //
    // Every control here is bound to state the rasteriser actually reads. Anything
    // that would only affect the path tracer, or nothing at all, is deliberately
    // not offered rather than presented as a slider that does nothing.
    if (ImGui::CollapsingHeader("Coating & Cloth", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Applies the edit to every selected actor, and marks the scene dirty. The
        // pattern repeats for each parameter, so it is worth one lambda.
        const auto apply = [&](float Actor::*field, float value) {
            for (Actor* actor : selected_actors) actor->*field = value;
            scene_dirty = true;
        };

        float clearcoat = primary_actor->clearcoat;
        if (ImGui::SliderFloat("Clearcoat", &clearcoat, 0.0f, 1.0f, "%.3f")) {
            apply(&Actor::clearcoat, clearcoat);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("A second specular lobe over the base surface - car paint,\n"
                              "lacquered wood, varnish. Adds a highlight that stays sharp\n"
                              "even when the material underneath is rough.");
        }

        float clearcoat_roughness = primary_actor->clearcoat_roughness;
        if (ImGui::SliderFloat("Clearcoat Roughness", &clearcoat_roughness, 0.0f, 1.0f, "%.3f")) {
            apply(&Actor::clearcoat_roughness, clearcoat_roughness);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("How polished the coat itself is. Low is a mirror finish;\n"
                              "high is the soft sheen of a worn or brushed lacquer.");
        }

        float sheen = primary_actor->sheen;
        if (ImGui::SliderFloat("Sheen", &sheen, 0.0f, 1.0f, "%.3f")) {
            apply(&Actor::sheen, sheen);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Retroreflection at grazing angles - the bright rim cloth\n"
                              "picks up along a silhouette. Velvet, felt, brushed fabric.");
        }

        float subsurface = primary_actor->subsurface;
        if (ImGui::SliderFloat("Subsurface", &subsurface, 0.0f, 1.0f, "%.3f")) {
            apply(&Actor::subsurface, subsurface);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Light entering the surface and scattering back out.\n"
                              "Skin, wax, marble, leaves held against the sun.");
        }

        float specular_tint = primary_actor->specular_tint;
        if (ImGui::SliderFloat("Specular Tint", &specular_tint, 0.0f, 1.0f, "%.3f")) {
            apply(&Actor::specular_tint, specular_tint);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pulls the specular highlight toward the albedo's hue.\n"
                              "A pure dielectric reflects white; varnish over red wood\n"
                              "throws a faintly warm highlight. No effect on metal,\n"
                              "which takes its specular colour from albedo anyway.");
        }

        float normal_strength = primary_actor->normal_strength;
        if (ImGui::SliderFloat("Normal Strength", &normal_strength, 0.0f, 4.0f, "%.2f")) {
            apply(&Actor::normal_strength, normal_strength);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Scales the normal map's detail. 0 is the bare geometric\n"
                              "surface, 1 is the map as authored, above that exaggerates.\n"
                              "Only does anything on a mesh that found a normal map -\n"
                              "one named after its albedo with a _normal suffix.");
        }

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        if (ImGui::Button("Reset Surface", ImVec2(-FLT_MIN, 0.0f))) {
            for (Actor* actor : selected_actors) {
                actor->clearcoat = 0.0f;
                actor->clearcoat_roughness = 0.1f;
                actor->sheen = 0.0f;
                actor->subsurface = 0.0f;
                actor->normal_strength = 1.0f;
                actor->specular_tint = 0.0f;
                actor->emission_color = { 1.0f, 1.0f, 1.0f };
            }
            scene_dirty = true;
        }

        // Presets. Getting a convincing material out of five numbers is mostly a
        // matter of knowing which combination reads as which substance, which is
        // exactly the knowledge a first-time user does not have yet.
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::TextDisabled("Presets");
        struct SurfacePreset {
            const char* name;
            float metallic, roughness, clearcoat, clearcoat_roughness, sheen, subsurface;
        };
        static const SurfacePreset presets[] = {
            { "Plastic",    0.0f, 0.35f, 0.0f,  0.10f, 0.0f, 0.0f },
            { "Car Paint",  0.1f, 0.35f, 1.0f,  0.03f, 0.0f, 0.0f },
            { "Polished Metal", 1.0f, 0.10f, 0.0f, 0.10f, 0.0f, 0.0f },
            { "Brushed Metal",  1.0f, 0.45f, 0.0f, 0.10f, 0.0f, 0.0f },
            { "Velvet",     0.0f, 0.85f, 0.0f,  0.10f, 1.0f, 0.0f },
            { "Skin",       0.0f, 0.55f, 0.0f,  0.10f, 0.0f, 0.7f },
            { "Marble",     0.0f, 0.25f, 0.3f,  0.15f, 0.0f, 0.5f },
            { "Rubber",     0.0f, 0.95f, 0.0f,  0.10f, 0.0f, 0.0f },
            { "Varnished Wood", 0.0f, 0.55f, 0.8f, 0.08f, 0.0f, 0.0f },
        };
        const float button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        for (int i = 0; i < IM_ARRAYSIZE(presets); ++i) {
            const SurfacePreset& preset = presets[i];
            if (ImGui::Button(preset.name, ImVec2(button_width, 0.0f))) {
                for (Actor* actor : selected_actors) {
                    actor->metallic = preset.metallic;
                    actor->roughness = preset.roughness;
                    actor->clearcoat = preset.clearcoat;
                    actor->clearcoat_roughness = preset.clearcoat_roughness;
                    actor->sheen = preset.sheen;
                    actor->subsurface = preset.subsurface;
                }
                scene_dirty = true;
            }
            if (i % 2 == 0 && i + 1 < IM_ARRAYSIZE(presets)) ImGui::SameLine();
        }
    }

    for (auto& comp : primary_actor->get_components()) {
        if (auto light = dynamic_cast<LightComponent*>(comp.get())) {
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s Properties", light->get_name().c_str());
            ImGui::Separator();
            
            float light_col[3] = { light->color.x, light->color.y, light->color.z };
            if (ImGui::ColorEdit3("Light Color", light_col)) {
                light->color = { light_col[0], light_col[1], light_col[2] };
            }
            ImGui::SliderFloat("Light Intensity", &light->intensity, 0.0f, 50.0f);
            
            if (auto pt = dynamic_cast<PointLightComponent*>(light)) {
                ImGui::SliderFloat("Radius", &pt->radius, 0.1f, 100.0f);
            } else if (auto spt = dynamic_cast<SpotLightComponent*>(light)) {
                ImGui::SliderFloat("Inner Angle", &spt->inner_angle, 0.0f, 90.0f);
                ImGui::SliderFloat("Outer Angle", &spt->outer_angle, 0.0f, 90.0f);
            }
        }

        // Camera. Every field on the component drives the projection the game
        // renders through, and none of them had any UI at all - a camera actor
        // could be placed but not aimed, framed or ranged without editing the
        // scene file by hand.
        if (auto camera = dynamic_cast<CameraComponent*>(comp.get())) {
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Camera");
            ImGui::Separator();

            if (ImGui::Checkbox("Active", &camera->is_active)) scene_dirty = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("The camera the game renders through. Turning one on\n"
                                  "does not turn the others off - the first active one wins.");
            }

            if (ImGui::SliderFloat("Field of View", &camera->fov, 10.0f, 140.0f, "%.1f deg")) {
                scene_dirty = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Vertical field of view. ~50-70 reads as natural;\n"
                                  "above ~100 gives the stretched wide-angle look.");
            }

            // Dragged rather than slid: the useful range spans four orders of
            // magnitude, which no linear slider can address usefully.
            if (ImGui::DragFloat("Near Plane", &camera->near_plane, 0.005f, 0.001f, 10.0f, "%.3f m")) {
                // Keeping near below far matters: an inverted or degenerate range
                // makes the projection matrix singular and the view goes black.
                if (camera->near_plane >= camera->far_plane) {
                    camera->near_plane = camera->far_plane * 0.5f;
                }
                scene_dirty = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Nearest visible distance. Depth precision is spent\n"
                                  "mostly just past this plane, so raising it is the\n"
                                  "cheapest fix for z-fighting in the distance.");
            }

            if (ImGui::DragFloat("Far Plane", &camera->far_plane, 5.0f, 1.0f, 100000.0f, "%.0f m")) {
                if (camera->far_plane <= camera->near_plane) {
                    camera->far_plane = camera->near_plane * 2.0f;
                }
                scene_dirty = true;
            }

            ImGui::TextDisabled("Depth range ratio: %.0f:1", camera->far_plane / std::max(camera->near_plane, 1e-4f));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Far divided by near. Past roughly 10000:1 the depth\n"
                                  "buffer runs out of precision and distant surfaces\n"
                                  "start to z-fight.");
            }

            if (ImGui::Button("Reset Camera", ImVec2(-FLT_MIN, 0.0f))) {
                camera->fov = 45.0f;
                camera->near_plane = 0.1f;
                camera->far_plane = 1000.0f;
                scene_dirty = true;
            }
        }
    }
    
    if (auto sun = dynamic_cast<DirectionalLightActor*>(primary_actor)) {
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Rendering Overrides (Sun)");
        ImGui::Separator();
        
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "Global Illumination");
        if (active_renderer) {
            static const char* gi_labels[] = {
                "Off / Disabled",
                "Screen-Space GI (SSGI)",
                "Voxel Cone Tracing (VXGI)",
                "Hardware Ray Tracing"
            };
            int gi = static_cast<int>(active_renderer->gi_mode);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("##gi_mode", &gi, gi_labels, 4)) {
                active_renderer->gi_mode = static_cast<GIMode>(gi);
            }
            // Say plainly which tiers have a backend, rather than offering a mode
            // that silently does nothing.
            if (gi == 2 && !active_renderer->vxgi_available) {
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "This driver has no image");
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "load/store; running SSGI.");
            } else if (gi == 2) {
                // Grid settings, only once VXGI is both selected and usable.
                ImGui::SliderFloat("GI Intensity", &active_renderer->vxgi_intensity, 0.0f, 3.0f, "%.2f");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Scales the cone-traced bounce. Physically this is 1;\n"
                                      "a voxelised scene over-occludes slightly, and this is\n"
                                      "the trim for that.");
                }

                static const char* voxel_res_labels[] = { "32", "64", "128", "256" };
                static const int voxel_res_values[] = { 32, 64, 128, 256 };
                int voxel_res_index = 2;
                for (int i = 0; i < 4; ++i) {
                    if (voxel_res_values[i] == active_renderer->voxel_resolution) voxel_res_index = i;
                }
                if (ImGui::Combo("Voxel Grid", &voxel_res_index, voxel_res_labels, 4)) {
                    active_renderer->voxel_resolution = voxel_res_values[voxel_res_index];
                    // Reallocates the grid and forces a rebuild on the next frame.
                    active_renderer->voxel_volume_dirty = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Cubed, so 256 is 16.7M voxels and about eight times\n"
                                      "the memory and fill cost of 128. Drop this first on a\n"
                                      "small GPU.");
                }

                if (ImGui::SliderFloat("Grid Extent", &active_renderer->voxel_world_extent,
                                       8.0f, 256.0f, "%.0f m")) {
                    active_renderer->voxel_volume_dirty = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("World size the grid covers, centred on the camera.\n"
                                      "Extent divided by grid size is the voxel size, which\n"
                                      "is the finest detail that can bounce light.");
                }

                ImGui::TextDisabled("Voxel size: %.3f m",
                                    active_renderer->voxel_world_extent /
                                        std::max(active_renderer->voxel_resolution, 1));
                if (ImGui::Button("Rebuild Voxel Volume", ImVec2(-FLT_MIN, 0.0f))) {
                    active_renderer->voxel_volume_dirty = true;
                }
                ImGui::TextDisabled("Rebuilds when the camera leaves the\nregion, or on demand here.");
            } else if (gi == 3 && !Renderer::hardware_rt_supported) {
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "Needs a Vulkan/DX12 RT");
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "device; running SSGI.");
            }
        }

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        if (ImGui::Button("Bake Static Lighting", ImVec2(-FLT_MIN, 30.0f))) {
            if (g_engine) g_engine->bake_static_lighting();
        }
        ImGui::TextDisabled("Bakes every actor marked Static.\nWrites Content/Bakes/.");

        // --- Shadows, fog and post ------------------------------------------
        //
        // All of this was already live and tunable in the renderer, and none of it
        // was reachable without editing a source file and rebuilding. Each control
        // writes the renderer field directly, so the viewport updates on the frame
        // the slider moves.
        //
        // These are session settings, not scene settings: they live on the renderer
        // rather than on the sun actor, so they are not written into the .lithium
        // file. Changing them is for looking at the scene, not for shipping it.
        if (active_renderer) {
            Renderer& r = *active_renderer;

            if (ImGui::CollapsingHeader("Shadows")) {
                static const char* resolutions[] = { "512", "1024", "2048", "4096" };
                static const int resolution_values[] = { 512, 1024, 2048, 4096 };
                int resolution_index = 2;
                for (int i = 0; i < 4; ++i) {
                    if (resolution_values[i] == r.shadow_map_resolution) resolution_index = i;
                }
                if (ImGui::Combo("Resolution", &resolution_index, resolutions, 4)) {
                    // Reallocates the cascade array on the next frame.
                    r.shadow_map_resolution = resolution_values[resolution_index];
                }
                ImGui::TextDisabled("Per cascade, and there are %d of them.",
                                    Renderer::shadow_cascade_count());

                ImGui::SliderFloat("Distance", &r.shadow_distance, 20.0f, 1000.0f, "%.0f m");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Furthest a surface can be and still receive a shadow.\n"
                                      "Raising it spreads the same texels over more world,\n"
                                      "so near shadows get coarser.");
                }

                ImGui::SliderFloat("Split Lambda", &r.shadow_split_lambda, 0.0f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("0 spaces the cascade boundaries evenly, 1 spaces them\n"
                                      "logarithmically. Even starves the near field; fully\n"
                                      "logarithmic spends a whole cascade on the first metre.");
                }

                ImGui::SliderFloat("Cascade Blend", &r.shadow_cascade_blend, 0.0f, 0.5f, "%.3f");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Width of the cross-fade at a cascade boundary, as a\n"
                                      "fraction of the cascade. At 0 the change in texel\n"
                                      "density draws a visible line across the ground.");
                }

                ImGui::SliderFloat("Depth Bias", &r.shadow_depth_bias_texels, 0.0f, 8.0f, "%.2f texels");
                ImGui::SliderFloat("Normal Bias", &r.shadow_normal_bias_texels, 0.0f, 8.0f, "%.2f texels");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Both are in shadow-map texels, not world units - a texel\n"
                                      "is the only unit that means the same thing in all four\n"
                                      "cascades. Too little gives acne, too much peter-panning.");
                }

                ImGui::Checkbox("Debug: tint cascades", &r.debug_shadow_cascades);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Colours each cascade differently so the splits can be\n"
                                      "seen rather than guessed at.");
                }

                if (ImGui::Button("Reset Shadows", ImVec2(-FLT_MIN, 0.0f))) {
                    r.shadow_map_resolution = 2048;
                    r.shadow_distance = 250.0f;
                    r.shadow_split_lambda = 0.85f;
                    r.shadow_cascade_blend = 0.15f;
                    r.shadow_depth_bias_texels = 1.6f;
                    r.shadow_normal_bias_texels = 2.2f;
                    r.debug_shadow_cascades = false;
                }
            }

            if (ImGui::CollapsingHeader("Atmosphere & Fog")) {
                ImGui::SliderFloat("Fog Density", &r.fog_density, 0.0f, 0.2f, "%.4f");
                ImGui::SliderFloat("Fog Height", &r.fog_height, -100.0f, 200.0f, "%.1f m");
                ImGui::SliderFloat("Height Falloff", &r.fog_height_falloff, 0.0f, 1.0f, "%.3f");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("How quickly the fog thins with altitude. At 0 it fills\n"
                                      "the world evenly; higher values keep it pooled low.");
                }
                if (ImGui::Button("Reset Fog", ImVec2(-FLT_MIN, 0.0f))) {
                    r.fog_density = 0.018f;
                    r.fog_height = 0.0f;
                    r.fog_height_falloff = 0.12f;
                }
            }

            if (ImGui::CollapsingHeader("Ambient Occlusion & Reflections")) {
                ImGui::SliderFloat("AO Strength", &r.ssao_strength, 0.0f, 2.0f, "%.2f");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Contact darkening in creases and where objects meet.\n"
                                      "Applied to indirect light only, not to the whole image.");
                }
                ImGui::Checkbox("Screen-Space Reflections", &r.enable_ssr);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Reflections of what is on screen. Off-screen geometry\n"
                                      "cannot reflect, so reflections fade toward the edges -\n"
                                      "that is inherent to the technique, not a bug.");
                }
            }

            if (ImGui::CollapsingHeader("Exposure & Presentation")) {
                ImGui::SliderFloat("Exposure Bias", &r.exposure_bias, 0.1f, 4.0f, "%.2f");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Multiplies the auto-exposure result. This is the\n"
                                      "brightness control a viewer actually wants.");
                }
                ImGui::Checkbox("Wireframe", &r.wireframe_mode);
                if (ImGui::Button("Reset Exposure", ImVec2(-FLT_MIN, 0.0f))) {
                    r.exposure_bias = 1.0f;
                }
            }

            if (ImGui::CollapsingHeader("Culling & Performance")) {
                ImGui::Checkbox("Frustum Culling", &r.enable_frustum_culling);
                ImGui::Checkbox("Occlusion Culling", &r.enable_occlusion_culling);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Skips objects hidden behind others, using the previous\n"
                                      "frame's queries. Off by default: it costs a query per\n"
                                      "object and only pays for itself in dense scenes.");
                }
                // Read-only counters, so the effect of the two toggles above is
                // visible rather than having to be taken on faith.
                ImGui::TextDisabled("Culled last frame: %d frustum, %d occlusion",
                                    r.culled_by_frustum, r.culled_by_occlusion);
            }
        }

        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "Sky / Background");
        static const char* sky_modes[] = { "Environment HDRI", "Procedural Sky", "Void Color" };
        if (ImGui::Combo("Sky Mode", &sun->sky_mode, sky_modes, 3)) scene_dirty = true;

        if (sun->sky_mode == 0) {
            // Sky library. Scanned from disk rather than hard-coded, so dropping more
            // .hdr files into EngineContent/Skies makes them available with no rebuild.
            static std::vector<std::string> sky_files;
            static std::vector<std::string> sky_labels;
            static bool sky_scanned = false;
            if (!sky_scanned) {
                sky_scanned = true;
                sky_files.clear();
                sky_labels.clear();
                const char* dirs[] = { "EngineContent/Skies", "EngineContent" };
                for (const char* d : dirs) {
                    std::error_code ec;
                    if (!std::filesystem::exists(d, ec)) continue;
                    for (const auto& e : std::filesystem::directory_iterator(d, ec)) {
                        if (!e.is_regular_file()) continue;
                        if (e.path().extension() != ".hdr") continue;
                        sky_files.push_back(e.path().generic_string());
                        // Turn "kloofendal_48d_partly_cloudy" into something readable.
                        std::string label = e.path().stem().string();
                        std::replace(label.begin(), label.end(), '_', ' ');
                        if (!label.empty()) label[0] = static_cast<char>(std::toupper(label[0]));
                        sky_labels.push_back(label);
                    }
                }
                // Stable alphabetical order so the list doesn't shuffle between runs.
                std::vector<size_t> order(sky_files.size());
                for (size_t i = 0; i < order.size(); ++i) order[i] = i;
                std::sort(order.begin(), order.end(),
                          [&](size_t a, size_t b) { return sky_labels[a] < sky_labels[b]; });
                std::vector<std::string> f2, l2;
                for (size_t i : order) { f2.push_back(sky_files[i]); l2.push_back(sky_labels[i]); }
                sky_files.swap(f2);
                sky_labels.swap(l2);
            }

            if (sky_files.empty()) {
                ImGui::TextDisabled("No .hdr files found in EngineContent/Skies.");
            } else {
                ImGui::Text("Sky Library  (%zu)", sky_files.size());
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##sky_library", sky_label_current.empty() ? "Select a sky..." : sky_label_current.c_str())) {
                    for (size_t i = 0; i < sky_files.size(); ++i) {
                        bool selected = (sky_label_current == sky_labels[i]);
                        if (ImGui::Selectable(sky_labels[i].c_str(), selected)) {
                            sky_label_current = sky_labels[i];
                            if (g_engine) g_engine->set_sky_hdri(sky_files[i]);
                            scene_dirty = true;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("CC0 / public domain. Drop more .hdr files in\nEngineContent/Skies to extend the library.");
            }
        }
        if (sun->sky_mode == 1) {
            if (ImGui::Checkbox("Volumetric 3D Clouds", &sun->enable_3d_clouds)) scene_dirty = true;
        }
        if (sun->sky_mode == 2) {
            float vc[3] = { sun->void_color.x, sun->void_color.y, sun->void_color.z };
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::ColorEdit3("##void_color", vc, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel)) {
                sun->void_color = { vc[0], vc[1], vc[2] };
                scene_dirty = true;
            }
            ImGui::TextDisabled("Flat background colour behind the scene.");
        }
        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        ImGui::Checkbox("Enable Hardware MSAA", &sun->enable_msaa);
        ImGui::Checkbox("Enable Temporal Anti-Aliasing (TAA)", &sun->enable_taa);
        if (sun->enable_taa) {
            ImGui::SliderFloat("Internal Render Scale (Upscaling)", &sun->upscaling_scale, 0.25f, 1.0f);
        }
        ImGui::Checkbox("Simulated Ray Traced Shadows", &sun->enable_ray_tracing);
        // The Embree toggle that used to sit here drove the old path tracer's
        // acceleration structure. TESLA now uses its own BVH on both backends, so
        // there is nothing left for the flag to select; its settings live under
        // Rendering -> TESLA Settings.
        ImGui::Checkbox("Enable 3D Volumetric Clouds", &sun->enable_3d_clouds);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Procedural cloud layer in the rasterised sky.");
        }
    }


    
    if (auto sprite = dynamic_cast<SpriteActor*>(primary_actor)) {
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Sprite Attributes");
        ImGui::Separator();
        
        static char tex_buf[256] = "";
        ImGui::InputText("Sprite Texture", tex_buf, sizeof(tex_buf));
        if (ImGui::Button("Apply Texture")) {
            sprite->set_texture(tex_buf);
        }
    }

    // Display Attached Attributes (Components)
    for (auto& comp : primary_actor->get_components()) {
        if (auto phys = dynamic_cast<PhysicsAttribute*>(comp.get())) {
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Physics Attribute");
            ImGui::Separator();
            
            ImGui::DragFloat("Mass (0 = Static)", &phys->mass, 0.1f, 0.0f, 1000.0f);
            ImGui::DragFloat("Friction", &phys->friction, 0.05f, 0.0f, 1.0f);
            ImGui::DragFloat("Restitution (Bounciness)", &phys->restitution, 0.05f, 0.0f, 1.0f);
            ImGui::Checkbox("Simulate Gravity", &phys->simulate_gravity);
            ImGui::Checkbox("Is Trigger", &phys->is_trigger);
            draw_layer_combo("Collision Layer", phys->collision_layer);
            if (phys->is_trigger) {
                ImGui::TextDisabled("Reports overlap only; nothing is pushed.");
                ImGui::TextDisabled("Scripts receive on_trigger_enter/stay/exit.");
            }
            
            const char* shapes[PhysicsAttribute::Collider_Count];
            for (int i = 0; i < PhysicsAttribute::Collider_Count; ++i) {
                shapes[i] = PhysicsAttribute::collider_type_name(i);
            }
            ImGui::Combo("Collider Shape", &phys->collider_type, shapes, PhysicsAttribute::Collider_Count);

            switch (phys->collider_type) {
                case PhysicsAttribute::Collider_Box: {
                    float extents[3] = { phys->box_half_extents.x, phys->box_half_extents.y, phys->box_half_extents.z };
                    if (ImGui::DragFloat3("Half Extents", extents, 0.1f)) {
                        phys->box_half_extents = { extents[0], extents[1], extents[2] };
                    }
                    break;
                }
                case PhysicsAttribute::Collider_Sphere:
                    ImGui::DragFloat("Radius", &phys->sphere_radius, 0.1f, 0.01f, 100.0f);
                    break;
                case PhysicsAttribute::Collider_Capsule:
                    ImGui::DragFloat("Radius##cap", &phys->capsule_radius, 0.05f, 0.01f, 100.0f);
                    ImGui::DragFloat("Half Height##cap", &phys->capsule_half_height, 0.05f, 0.01f, 100.0f);
                    ImGui::TextDisabled("Total height: %.2f", 2.0f * (phys->capsule_half_height + phys->capsule_radius));
                    break;
                case PhysicsAttribute::Collider_Cylinder:
                    ImGui::DragFloat("Radius##cyl", &phys->cylinder_radius, 0.05f, 0.01f, 100.0f);
                    ImGui::DragFloat("Half Height##cyl", &phys->cylinder_half_height, 0.05f, 0.01f, 100.0f);
                    break;
                case PhysicsAttribute::Collider_ConvexHull:
                    ImGui::TextDisabled("Built from this actor's mesh.");
                    break;
                case PhysicsAttribute::Collider_Mesh:
                    ImGui::TextDisabled("Built from this actor's mesh.");
                    ImGui::TextDisabled("Triangle meshes are always static.");
                    break;
                default:
                    break;
            }

            if (!phys->get_status().empty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1.0f), "%s", phys->get_status().c_str());
            }
        } else if (auto lua_script = dynamic_cast<LuaScriptComponent*>(comp.get())) {
            ImGui::PushID(comp.get());
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Lua Script");
            ImGui::Separator();

            char path_buf[256];
            strncpy(path_buf, lua_script->script_path.c_str(), sizeof(path_buf));
            path_buf[sizeof(path_buf) - 1] = '\0';
            if (ImGui::InputText("Script (.lua)", path_buf, sizeof(path_buf))) {
                lua_script->script_path = path_buf;
                lua_script->reload();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_FILE")) {
                    std::string dropped((const char*)payload->Data);
                    if (dropped.find(".lua") != std::string::npos) {
                        lua_script->script_path = dropped;
                        lua_script->reload();
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (ImGui::Button("Reload")) lua_script->reload();

            if (lua_script->has_error()) {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "Error:");
                ImGui::TextWrapped("%s", lua_script->get_last_error().c_str());
            }

            // Whatever the script declared in its `properties` table. This is the
            // whole point: a designer tunes the script without opening it.
            const auto& declared = lua_script->get_declared_properties();
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            if (declared.empty()) {
                ImGui::TextDisabled("No properties declared.");
                ImGui::TextDisabled("Add a `properties = { speed = 5.0 }` table to");
                ImGui::TextDisabled("the script to expose settings here.");
            } else {
                ImGui::TextDisabled("Properties");
                for (const LuaAPI::ScriptProperty& property : declared) {
                    ImGui::PushID(property.name.c_str());

                    // The authored value if there is one, otherwise the script's own
                    // default. Editing any widget creates the override.
                    LuaAPI::ScriptProperty current = property;
                    LuaAPI::ScriptProperty* existing = nullptr;
                    for (auto& stored : lua_script->property_overrides) {
                        if (stored.name == property.name && stored.type == property.type) {
                            existing = &stored;
                            current = stored;
                            break;
                        }
                    }

                    bool changed = false;
                    switch (property.type) {
                        case LuaAPI::ScriptProperty::Type::Number: {
                            float value = static_cast<float>(current.number_value);
                            if (ImGui::DragFloat(property.name.c_str(), &value, 0.05f)) {
                                current.number_value = value;
                                changed = true;
                            }
                            break;
                        }
                        case LuaAPI::ScriptProperty::Type::String: {
                            char text[256];
                            strncpy(text, current.string_value.c_str(), sizeof(text));
                            text[sizeof(text) - 1] = '\0';
                            if (ImGui::InputText(property.name.c_str(), text, sizeof(text))) {
                                current.string_value = text;
                                changed = true;
                            }
                            break;
                        }
                        case LuaAPI::ScriptProperty::Type::Boolean: {
                            bool value = current.boolean_value;
                            if (ImGui::Checkbox(property.name.c_str(), &value)) {
                                current.boolean_value = value;
                                changed = true;
                            }
                            break;
                        }
                    }

                    if (changed) {
                        if (existing) *existing = current;
                        else lua_script->property_overrides.push_back(current);
                        lua_script->apply_property_overrides();
                        scene_dirty = true;
                    }

                    if (existing) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Reset")) {
                            // Drops the override so the script's default takes over
                            // again, which is what makes a default meaningful.
                            for (auto it = lua_script->property_overrides.begin();
                                 it != lua_script->property_overrides.end(); ++it) {
                                if (it->name == property.name) {
                                    lua_script->property_overrides.erase(it);
                                    break;
                                }
                            }
                            lua_script->reload();
                            scene_dirty = true;
                        }
                    }
                    ImGui::PopID();
                }
            }
            ImGui::PopID();
        } else if (auto emitter = dynamic_cast<ParticleEmitterComponent*>(comp.get())) {
            ImGui::PushID(comp.get());
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Particle Emitter");
            ImGui::Separator();
            ImGui::TextDisabled("%zu alive", emitter->get_particles().size());

            ImGui::Checkbox("Emitting", &emitter->is_emitting);
            ImGui::DragFloat("Rate", &emitter->emit_rate, 1.0f, 0.0f, 5000.0f, "%.0f /s");
            ImGui::DragInt("Max Particles", &emitter->max_particles, 10.0f, 1, 200000);
            ImGui::DragInt("Burst Count", &emitter->burst_count, 1.0f, 0, 5000);
            if (emitter->burst_count > 0) {
                ImGui::DragFloat("Burst Interval", &emitter->burst_interval, 0.05f, 0.02f, 60.0f, "%.2f s");
            }
            const char* spaces[] = { "Local (follows emitter)", "World (left behind)" };
            ImGui::Combo("Simulation Space", &emitter->simulation_space, spaces, 2);

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::TextDisabled("Shape");
            const char* shapes[ParticleEmitterComponent::Shape_Count];
            for (int i = 0; i < ParticleEmitterComponent::Shape_Count; ++i) {
                shapes[i] = ParticleEmitterComponent::shape_name(i);
            }
            ImGui::Combo("Shape", &emitter->shape, shapes, ParticleEmitterComponent::Shape_Count);
            if (emitter->shape == ParticleEmitterComponent::Shape_Box) {
                float extents[3] = { emitter->shape_extents.x, emitter->shape_extents.y, emitter->shape_extents.z };
                if (ImGui::DragFloat3("Extents", extents, 0.05f, 0.0f, 100.0f)) {
                    emitter->shape_extents = { extents[0], extents[1], extents[2] };
                }
            } else if (emitter->shape != ParticleEmitterComponent::Shape_Point) {
                ImGui::DragFloat("Radius##shape", &emitter->shape_radius, 0.05f, 0.0f, 100.0f);
            }
            if (emitter->shape == ParticleEmitterComponent::Shape_Cone) {
                ImGui::DragFloat("Cone Angle", &emitter->cone_angle, 0.5f, 0.0f, 89.0f, "%.0f deg");
            }

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::TextDisabled("At birth (min / max)");
            ImGui::DragFloatRange2("Lifetime", &emitter->lifetime_min, &emitter->lifetime_max, 0.05f, 0.01f, 120.0f, "%.2f s");
            ImGui::DragFloatRange2("Speed", &emitter->speed_min, &emitter->speed_max, 0.1f, 0.0f, 500.0f, "%.2f");
            ImGui::DragFloatRange2("Size", &emitter->size_min, &emitter->size_max, 0.01f, 0.001f, 100.0f, "%.3f");
            ImGui::DragFloatRange2("Spin", &emitter->rotation_speed_min, &emitter->rotation_speed_max, 0.05f, -50.0f, 50.0f, "%.2f rad/s");

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::TextDisabled("Over lifetime");
            float start_col[3] = { emitter->start_color.x, emitter->start_color.y, emitter->start_color.z };
            if (ImGui::ColorEdit3("Start Color", start_col)) {
                emitter->start_color = { start_col[0], start_col[1], start_col[2] };
            }
            float end_col[3] = { emitter->end_color.x, emitter->end_color.y, emitter->end_color.z };
            if (ImGui::ColorEdit3("End Color", end_col)) {
                emitter->end_color = { end_col[0], end_col[1], end_col[2] };
            }
            ImGui::SliderFloat("Start Alpha", &emitter->start_alpha, 0.0f, 1.0f);
            ImGui::SliderFloat("End Alpha", &emitter->end_alpha, 0.0f, 1.0f);
            ImGui::DragFloat("Start Size x", &emitter->size_start_scale, 0.01f, 0.0f, 20.0f);
            ImGui::DragFloat("End Size x", &emitter->size_end_scale, 0.01f, 0.0f, 20.0f);
            ImGui::DragFloat("Gravity", &emitter->gravity, 0.05f, -50.0f, 50.0f);
            ImGui::DragFloat("Drag", &emitter->drag, 0.02f, 0.0f, 20.0f);
            float accel[3] = { emitter->acceleration.x, emitter->acceleration.y, emitter->acceleration.z };
            if (ImGui::DragFloat3("Wind", accel, 0.05f)) {
                emitter->acceleration = { accel[0], accel[1], accel[2] };
            }

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::TextDisabled("Rendering");
            const char* blends[] = { "Additive (fire, sparks)", "Alpha (smoke, dust)" };
            ImGui::Combo("Blend", &emitter->blend_mode, blends, 2);
            ImGui::DragFloat("Intensity", &emitter->intensity, 0.05f, 0.0f, 50.0f);
            char tex_buf[256];
            strncpy(tex_buf, emitter->texture_path.c_str(), sizeof(tex_buf));
            tex_buf[sizeof(tex_buf) - 1] = '\0';
            if (ImGui::InputText("Texture##particle", tex_buf, sizeof(tex_buf))) {
                emitter->texture_path = tex_buf;
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_FILE")) {
                    emitter->texture_path = std::string((const char*)payload->Data);
                }
                ImGui::EndDragDropTarget();
            }
            if (emitter->texture_path.empty()) {
                ImGui::TextDisabled("No texture: a soft round dot is generated.");
            }

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::TextDisabled("Collision");
            ImGui::Checkbox("Collide With World", &emitter->collision_enabled);
            if (emitter->collision_enabled) {
                ImGui::DragFloat("Bounce", &emitter->collision_bounce, 0.01f, 0.0f, 1.0f);
                ImGui::Checkbox("Die On Impact", &emitter->die_on_collision);
                ImGui::TextDisabled("Costs one raycast per particle per frame.");
            }

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::TextDisabled("Sub-emitter");
            char sub_buf[128];
            strncpy(sub_buf, emitter->sub_emitter_actor.c_str(), sizeof(sub_buf));
            sub_buf[sizeof(sub_buf) - 1] = '\0';
            if (ImGui::InputText("Actor##sub", sub_buf, sizeof(sub_buf))) {
                emitter->sub_emitter_actor = sub_buf;
            }
            const char* triggers[] = { "None", "On Death", "On Collision" };
            ImGui::Combo("Trigger", &emitter->sub_emitter_trigger, triggers, 3);
            if (emitter->sub_emitter_trigger != ParticleEmitterComponent::Sub_None) {
                ImGui::DragInt("Burst##sub", &emitter->sub_emitter_count, 1.0f, 1, 500);
                ImGui::TextDisabled("Fires a burst on that actor's emitter.");
            }
            ImGui::PopID();
        } else if (auto terrain = dynamic_cast<TerrainComponent*>(comp.get())) {
            ImGui::PushID(comp.get());
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Terrain");
            ImGui::Separator();
            draw_terrain_panel(terrain);
            ImGui::PopID();
        } else if (auto agent = dynamic_cast<NavAgentComponent*>(comp.get())) {
            ImGui::PushID(comp.get());
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Nav Agent");
            ImGui::Separator();

            ImGui::DragFloat("Speed", &agent->speed, 0.1f, 0.0f, 50.0f, "%.2f m/s");
            ImGui::DragFloat("Turn Speed", &agent->angular_speed, 5.0f, 0.0f, 2000.0f, "%.0f deg/s");
            ImGui::DragFloat("Stopping Distance", &agent->stopping_distance, 0.01f, 0.01f, 10.0f, "%.2f m");
            ImGui::DragFloat("Waypoint Tolerance", &agent->waypoint_tolerance, 0.01f, 0.01f, 10.0f, "%.2f m");
            ImGui::Checkbox("Rotate To Face", &agent->rotate_to_face);
            ImGui::Checkbox("Auto Repath", &agent->auto_repath);
            if (agent->auto_repath) {
                ImGui::DragFloat("Repath Interval", &agent->repath_interval, 0.05f, 0.05f, 10.0f, "%.2f s");
            }

            if (auto* character = primary_actor->get_component<CharacterControllerComponent>()) {
                ImGui::TextDisabled("Driving the character controller on this actor.");
                if (character->use_player_input) {
                    ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1.0f),
                                       "Character has Use Player Input on; it will\n"
                                       "overwrite the agent's movement every frame.");
                }
            } else {
                ImGui::TextDisabled("No character controller: the transform is moved directly.");
            }

            if (agent->has_path()) {
                ImGui::TextDisabled("%zu waypoints, %.1f m remaining",
                                    agent->get_path().size(), agent->remaining_distance());
            } else {
                ImGui::TextDisabled("No path.");
            }
            if (!agent->get_status().empty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1.0f), "%s", agent->get_status().c_str());
            }
            ImGui::PopID();
        } else if (auto lod = dynamic_cast<LODGroupComponent*>(comp.get())) {
            ImGui::PushID(comp.get());
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "LOD Group");
            ImGui::Separator();
            ImGui::TextDisabled("Thresholds are the object's on-screen height as a");
            ImGui::TextDisabled("fraction of the viewport, so they hold at any resolution.");

            ImGui::DragFloat("Min Detail Distance", &lod->minimum_detail_distance, 0.5f, 0.0f, 10000.0f, "%.1f m");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Closer than this, always draw full detail.\nStops a large object dropping LOD because the camera is inside it.");
            }
            ImGui::DragFloat("Cull Below", &lod->cull_screen_height, 0.001f, 0.0f, 0.5f, "%.3f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Screen height below which the object stops being drawn.\nZero never culls.");
            }

            const int selected_level = lod->get_last_selected_level();
            if (selected_level < 0) {
                ImGui::TextDisabled("Currently drawing: full detail");
            } else if (selected_level >= (int)lod->levels.size()) {
                ImGui::TextDisabled("Currently drawing: culled");
            } else {
                ImGui::TextDisabled("Currently drawing: LOD %d", selected_level + 1);
            }

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            int level_to_remove = -1;
            for (int i = 0; i < (int)lod->levels.size(); ++i) {
                ImGui::PushID(i);
                auto& level = lod->levels[i];
                ImGui::Text("LOD %d", i + 1);

                char mesh_buf[256];
                strncpy(mesh_buf, level.mesh_path.c_str(), sizeof(mesh_buf));
                mesh_buf[sizeof(mesh_buf) - 1] = '\0';
                if (ImGui::InputText("Mesh", mesh_buf, sizeof(mesh_buf))) {
                    level.mesh_path = mesh_buf;
                    // Force the resource to be looked up again; the old one belongs
                    // to whatever mesh was named before.
                    level.resource.reset();
                    level.requested = false;
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_FILE")) {
                        level.mesh_path = std::string((const char*)payload->Data);
                        level.resource.reset();
                        level.requested = false;
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::DragFloat("Screen Height", &level.screen_height, 0.002f, 0.001f, 1.0f, "%.3f");
                if (ImGui::SmallButton("Remove")) level_to_remove = i;
                ImGui::Separator();
                ImGui::PopID();
            }
            if (level_to_remove >= 0) {
                lod->levels.erase(lod->levels.begin() + level_to_remove);
            }

            if (ImGui::Button("Add Level")) {
                LODGroupComponent::LODLevel level;
                // Half the previous threshold, so an added level slots in below the
                // last one rather than on top of it.
                level.screen_height = lod->levels.empty() ? 0.25f
                                                          : lod->levels.back().screen_height * 0.5f;
                lod->levels.push_back(level);
            }

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::TextDisabled("Generate from this actor's mesh");
            ImGui::DragInt("Levels", &lod_generate_levels, 0.1f, 1, 6);
            ImGui::DragFloat("Keep Per Level", &lod_generate_ratio, 0.01f, 0.1f, 0.9f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Fraction of the previous level's triangles each\ngenerated level keeps.");
            }
            if (ImGui::Button("Generate LODs")) {
                lod_generate_report.clear();
                const std::string& source = primary_actor->mesh_path;
                if (source.empty()) {
                    lod_generate_report = "This actor has no imported mesh to reduce.";
                } else {
                    std::string error;
                    auto produced = MeshSimplifier::generate_lod_chain(
                        source, lod_generate_levels, lod_generate_ratio, error);
                    if (produced.empty()) {
                        lod_generate_report = error.empty() ? "Nothing was generated." : error;
                    } else {
                        // Replaces the level list rather than appending: generating
                        // twice should give the same result, not two chains.
                        lod->levels.clear();
                        float threshold = 0.25f;
                        for (const std::string& path : produced) {
                            LODGroupComponent::LODLevel level;
                            level.mesh_path = path;
                            level.screen_height = threshold;
                            lod->levels.push_back(level);
                            threshold *= 0.5f;
                        }
                        lod_generate_report = "Generated " + std::to_string(produced.size()) + " level(s).";
                    }
                }
            }
            if (!lod_generate_report.empty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f), "%s", lod_generate_report.c_str());
            }
            ImGui::PopID();
        } else if (auto canvas = dynamic_cast<UICanvasComponent*>(comp.get())) {
            ImGui::PushID(comp.get());
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "UI Canvas");
            ImGui::Separator();
            draw_ui_canvas_panel(canvas);
            ImGui::PopID();
        } else if (auto joint = dynamic_cast<JointComponent*>(comp.get())) {
            // Pushed by pointer because an actor may carry several joints - a
            // ragdoll bone connects to its parent and its child - and without a
            // unique scope every one of them would drive the first one's widgets.
            ImGui::PushID(comp.get());
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Joint");
            ImGui::Separator();

            const char* joint_types[JointComponent::Joint_Count];
            for (int i = 0; i < JointComponent::Joint_Count; ++i) {
                joint_types[i] = JointComponent::joint_type_name(i);
            }
            ImGui::Combo("Type", &joint->joint_type, joint_types, JointComponent::Joint_Count);

            char connected_buf[128];
            strncpy(connected_buf, joint->connected_actor.c_str(), sizeof(connected_buf));
            connected_buf[sizeof(connected_buf) - 1] = '\0';
            if (ImGui::InputText("Connected Actor", connected_buf, sizeof(connected_buf))) {
                joint->connected_actor = connected_buf;
            }
            if (joint->connected_actor.empty()) {
                ImGui::TextDisabled("Empty: anchored to the world.");
            }

            float anchor_v[3] = { joint->anchor.x, joint->anchor.y, joint->anchor.z };
            if (ImGui::DragFloat3("Anchor (local)", anchor_v, 0.05f)) {
                joint->anchor = { anchor_v[0], anchor_v[1], anchor_v[2] };
            }

            const bool needs_axis = joint->joint_type != JointComponent::Joint_Fixed &&
                                    joint->joint_type != JointComponent::Joint_Point &&
                                    joint->joint_type != JointComponent::Joint_Distance;
            if (needs_axis) {
                float axis_v[3] = { joint->axis.x, joint->axis.y, joint->axis.z };
                if (ImGui::DragFloat3("Axis (local)", axis_v, 0.05f)) {
                    joint->axis = { axis_v[0], axis_v[1], axis_v[2] };
                }
            }

            if (JointComponent::joint_type_has_limits(joint->joint_type)) {
                ImGui::Dummy(ImVec2(0.0f, 6.0f));
                ImGui::TextDisabled("Limits");
                ImGui::Checkbox("Enable Limits", &joint->enable_limits);
                if (joint->enable_limits) {
                    const bool angular = (joint->joint_type == JointComponent::Joint_Hinge);
                    ImGui::DragFloat(angular ? "Min (deg)" : "Min (m)", &joint->limit_min, 1.0f,
                                     angular ? -180.0f : -1000.0f, angular ? 180.0f : 1000.0f);
                    ImGui::DragFloat(angular ? "Max (deg)" : "Max (m)", &joint->limit_max, 1.0f,
                                     angular ? -180.0f : -1000.0f, angular ? 180.0f : 1000.0f);
                }
            }

            if (joint->joint_type == JointComponent::Joint_Distance) {
                ImGui::Dummy(ImVec2(0.0f, 6.0f));
                ImGui::TextDisabled("Distance (negative = keep current separation)");
                ImGui::DragFloat("Min Distance", &joint->min_distance, 0.05f, -1.0f, 1000.0f);
                ImGui::DragFloat("Max Distance", &joint->max_distance, 0.05f, -1.0f, 1000.0f);
            }

            if (joint->joint_type == JointComponent::Joint_Cone ||
                joint->joint_type == JointComponent::Joint_SwingTwist) {
                ImGui::Dummy(ImVec2(0.0f, 6.0f));
                ImGui::TextDisabled("Swing / twist");
                ImGui::DragFloat("Swing Half-Angle (deg)", &joint->swing_angle, 1.0f, 0.0f, 180.0f);
                if (joint->joint_type == JointComponent::Joint_SwingTwist) {
                    ImGui::DragFloat("Twist Min (deg)", &joint->twist_min, 1.0f, -180.0f, 180.0f);
                    ImGui::DragFloat("Twist Max (deg)", &joint->twist_max, 1.0f, -180.0f, 180.0f);
                }
            }

            const bool springable = JointComponent::joint_type_has_limits(joint->joint_type) ||
                                    joint->joint_type == JointComponent::Joint_Distance;
            if (springable) {
                ImGui::Dummy(ImVec2(0.0f, 6.0f));
                ImGui::TextDisabled("Spring");
                ImGui::Checkbox("Enable Spring", &joint->enable_spring);
                if (joint->enable_spring) {
                    ImGui::DragFloat("Frequency (Hz)", &joint->spring_frequency, 0.1f, 0.0f, 60.0f);
                    ImGui::DragFloat("Damping", &joint->spring_damping, 0.05f, 0.0f, 10.0f);
                }
            }

            if (JointComponent::joint_type_has_motor(joint->joint_type)) {
                const bool angular = (joint->joint_type == JointComponent::Joint_Hinge);
                ImGui::Dummy(ImVec2(0.0f, 6.0f));
                ImGui::TextDisabled("Motor");
                ImGui::Checkbox("Enable Motor", &joint->enable_motor);
                ImGui::DragFloat(angular ? "Target (rad/s)" : "Target (m/s)",
                                 &joint->motor_target_velocity, 0.05f, -100.0f, 100.0f);
                ImGui::DragFloat(angular ? "Max Torque (N m)" : "Max Force (N)",
                                 &joint->motor_max_force, 5.0f, 0.0f, 1000000.0f);
                ImGui::DragFloat(angular ? "Friction (N m)" : "Friction (N)",
                                 &joint->friction, 1.0f, 0.0f, 100000.0f);
                if (joint->has_joint()) {
                    ImGui::TextDisabled(angular ? "Current angle: %.1f deg" : "Current offset: %.2f m",
                                        joint->get_current_value());
                }
            }

            if (!joint->get_status().empty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1.0f), "%s", joint->get_status().c_str());
            }
            ImGui::PopID();
        } else if (auto character = dynamic_cast<CharacterControllerComponent*>(comp.get())) {
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Character Controller");
            ImGui::Separator();

            ImGui::TextDisabled("Capsule");
            ImGui::DragFloat("Radius##char", &character->capsule_radius, 0.05f, 0.01f, 10.0f);
            ImGui::DragFloat("Half Height##char", &character->capsule_half_height, 0.05f, 0.01f, 10.0f);
            ImGui::TextDisabled("Total height: %.2f", 2.0f * (character->capsule_half_height + character->capsule_radius));

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::TextDisabled("Movement");
            ImGui::DragFloat("Walk Speed", &character->walk_speed, 0.1f, 0.0f, 100.0f);
            ImGui::DragFloat("Sprint Multiplier", &character->sprint_multiplier, 0.05f, 1.0f, 10.0f);
            ImGui::DragFloat("Jump Speed", &character->jump_speed, 0.1f, 0.0f, 100.0f);
            ImGui::DragFloat("Gravity Scale", &character->gravity_scale, 0.05f, 0.0f, 10.0f);
            ImGui::DragFloat("Max Slope (deg)", &character->max_slope_angle, 1.0f, 0.0f, 89.0f);
            ImGui::DragFloat("Step Height", &character->step_height, 0.05f, 0.0f, 5.0f);
            draw_layer_combo("Collision Layer##char", character->collision_layer);

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::TextDisabled("Input");
            ImGui::Checkbox("Use Player Input", &character->use_player_input);
            if (character->use_player_input) {
                ImGui::Checkbox("Mouse Look", &character->mouse_look);
                if (character->mouse_look) {
                    ImGui::DragFloat("Sensitivity", &character->mouse_sensitivity, 0.0001f, 0.0001f, 0.05f, "%.4f");
                }
                ImGui::TextDisabled("Bound to MoveForward / MoveRight / Jump / Sprint.");
            } else {
                ImGui::TextDisabled("Driven from script via set_move_input().");
            }

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            Vector3 v = character->get_velocity();
            ImGui::TextDisabled("Grounded: %s   Speed: %.2f",
                                character->is_grounded() ? "yes" : "no",
                                std::sqrt(v.x * v.x + v.z * v.z));
        } else if (auto audio = dynamic_cast<AudioComponent*>(comp.get())) {
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Audio Component");
            ImGui::Separator();
            
            char path_buf[256];
            strncpy(path_buf, audio->get_file_path().c_str(), sizeof(path_buf));
            if (ImGui::InputText("Audio File", path_buf, sizeof(path_buf))) {
                audio->set_file_path(path_buf);
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_FILE")) {
                    std::string filepath((const char*)payload->Data);
                    if (filepath.find(".wav") != std::string::npos || filepath.find(".mp3") != std::string::npos || filepath.find(".ogg") != std::string::npos || filepath.find(".flac") != std::string::npos) {
                        audio->set_file_path(filepath);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (ImGui::Button("Browse...##audio")) {
                auto selection = pfd::open_file("Select Audio File", ".",
                                                { "Audio Files", "*.wav *.mp3 *.flac *.ogg",
                                                  "All Files", "*" },
                                                pfd::opt::none).result();
                if (!selection.empty()) {
                    audio->set_file_path(selection[0]);
                }
            }
            
            bool is_looping = audio->get_looping();
            if (ImGui::Checkbox("Looping", &is_looping)) audio->set_looping(is_looping);
            
            bool is_spatial = audio->get_spatial();
            if (ImGui::Checkbox("3D Spatialization", &is_spatial)) audio->set_spatial(is_spatial);
            
            float vol = audio->get_volume();
            if (ImGui::SliderFloat("Volume", &vol, 0.0f, 1.0f)) audio->set_volume(vol);
            
            float pitch = audio->get_pitch();
            if (ImGui::SliderFloat("Pitch", &pitch, 0.1f, 3.0f)) audio->set_pitch(pitch);

            // Mixer routing. Changing it re-creates the sound, because miniaudio
            // binds a sound to its group at creation time.
            {
                AudioEngine& engine_audio = AudioEngine::get();
                std::string preview = engine_audio.get_bus_name(audio->bus);
                if (ImGui::BeginCombo("Bus", preview.c_str())) {
                    for (int candidate = 0; candidate < AudioEngine::kBusCount; ++candidate) {
                        const bool selected = (candidate == audio->bus);
                        if (ImGui::Selectable(engine_audio.get_bus_name(candidate).c_str(), selected)) {
                            audio->bus = candidate;
                            audio->reload_sound();
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            if (audio->get_spatial()) {
                ImGui::Dummy(ImVec2(0.0f, 6.0f));
                ImGui::TextDisabled("3D");
                bool spatial_changed = false;
                const char* models[] = { "None", "Inverse", "Linear", "Exponential" };
                spatial_changed |= ImGui::Combo("Attenuation", &audio->attenuation_model, models, 4);
                spatial_changed |= ImGui::DragFloat("Min Distance", &audio->min_distance, 0.1f, 0.01f, 1000.0f, "%.2f m");
                spatial_changed |= ImGui::DragFloat("Max Distance", &audio->max_distance, 0.5f, 0.02f, 10000.0f, "%.2f m");
                spatial_changed |= ImGui::DragFloat("Rolloff", &audio->rolloff, 0.05f, 0.0f, 10.0f, "%.2f");
                spatial_changed |= ImGui::DragFloat("Doppler", &audio->doppler_factor, 0.05f, 0.0f, 5.0f, "%.2f");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Pitch shift from relative motion. 0 is off, 1 is physically correct.");
                }
                ImGui::TextDisabled("Cone (360 both = omnidirectional)");
                spatial_changed |= ImGui::DragFloat("Inner Angle", &audio->cone_inner_angle, 1.0f, 0.0f, 360.0f, "%.0f deg");
                spatial_changed |= ImGui::DragFloat("Outer Angle", &audio->cone_outer_angle, 1.0f, 0.0f, 360.0f, "%.0f deg");
                spatial_changed |= ImGui::DragFloat("Outer Gain", &audio->cone_outer_gain, 0.01f, 0.0f, 1.0f, "%.2f");
                if (spatial_changed) audio->apply_spatial_settings();
            }

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            if (ImGui::Button("Play")) audio->play();
            ImGui::SameLine();
            if (ImGui::Button("Stop")) audio->stop();
        } else if (auto cpp_script = dynamic_cast<CppScriptComponent*>(comp.get())) {
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "C++ Script Component");
            ImGui::Separator();
            
            char path_buf[256];
            strncpy(path_buf, cpp_script->script_path.c_str(), sizeof(path_buf));
            if (ImGui::InputText("Script Path (.cpp)", path_buf, sizeof(path_buf))) {
                cpp_script->script_path = path_buf;
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_FILE")) {
                    std::string filepath((const char*)payload->Data);
                    if (filepath.find(".cpp") != std::string::npos) {
                        cpp_script->script_path = filepath;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (ImGui::Button("Compile & Load", ImVec2(ImGui::GetContentRegionAvail().x, 30.0f))) {
                cpp_script->compile_and_load();
            }

            if (!cpp_script->build_log.empty()) {
                if (cpp_script->has_error) {
                    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Build Error:");
                } else {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Build Success:");
                }
                ImGui::BeginChild("BuildLog", ImVec2(0, 100), true);
                ImGui::TextWrapped("%s", cpp_script->build_log.c_str());
                ImGui::EndChild();
            }
        }
    }

    // Skeletal animation. Only appears once the actor's mesh has finished loading
    // and turned out to carry a skeleton - there is nothing to drive on a static
    // mesh, and the player does not exist for one.
    if (auto* mesh_comp = primary_actor->get_component<StaticMeshComponent>()) {
        if (AnimationPlayer* animator = mesh_comp->get_animator()) {
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Animation");
            ImGui::TextDisabled("%d bones", animator->get_bone_count());

            const int clip_count = animator->get_clip_count();
            if (clip_count == 0) {
                ImGui::TextDisabled("Skeleton has no animation clips.");
            } else {
                int current = animator->get_current_clip();
                const char* preview = (current >= 0) ? animator->get_clip_name(current).c_str() : "(none)";
                if (ImGui::BeginCombo("Clip", preview)) {
                    for (int i = 0; i < clip_count; ++i) {
                        bool is_selected = (i == current);
                        if (ImGui::Selectable(animator->get_clip_name(i).c_str(), is_selected)) {
                            animator->play(i, animator->is_looping());
                        }
                        if (is_selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                bool playing = animator->is_playing();
                if (ImGui::Button(playing ? "Pause" : "Play", ImVec2(70.0f, 0.0f))) {
                    animator->set_playing(!playing);
                }
                ImGui::SameLine();
                if (ImGui::Button("Stop", ImVec2(70.0f, 0.0f))) {
                    animator->stop();
                }
                ImGui::SameLine();
                bool looping = animator->is_looping();
                if (ImGui::Checkbox("Loop", &looping)) {
                    animator->set_looping(looping);
                }

                float speed = animator->get_speed();
                if (ImGui::DragFloat("Speed", &speed, 0.01f, -4.0f, 4.0f, "%.2fx")) {
                    animator->set_speed(speed);
                }

                // Scrubbing pauses: dragging the cursor while the clip is still
                // advancing fights the playhead and the handle will not stay put.
                float duration = animator->get_duration_seconds();
                if (duration > 0.0f) {
                    float time = animator->get_time_seconds();
                    if (ImGui::SliderFloat("Time", &time, 0.0f, duration, "%.2fs")) {
                        animator->set_playing(false);
                        animator->set_time_seconds(time);
                    }
                }

                // --- Blending ---------------------------------------------
                // Every clip in the asset is listed with its own weight, layer and
                // blend mode, because that is what the runtime actually evaluates:
                // the pose on screen is their weighted sum, not whichever clip the
                // combo box happens to name.
                ImGui::Dummy(ImVec2(0.0f, 6.0f));
                if (ImGui::CollapsingHeader("Blending")) {
                    ImGui::DragFloat("Fade (s)", &animation_fade_seconds, 0.01f, 0.0f, 4.0f, "%.2fs");
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Duration used by the Fade To buttons below.\n"
                                          "Zero is a hard cut.");
                    }

                    ImGui::TextDisabled("%d clip(s) contributing", animator->get_active_state_count());

                    if (ImGui::BeginTable("AnimStates", 5,
                                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("Clip", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                        ImGui::TableSetupColumn("Weight", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                        ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                        ImGui::TableSetupColumn("Add", ImGuiTableColumnFlags_WidthStretch, 0.8f);
                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                        ImGui::TableHeadersRow();

                        for (int i = 0; i < clip_count; ++i) {
                            ImGui::TableNextRow();
                            ImGui::PushID(i);

                            ImGui::TableSetColumnIndex(0);
                            const bool contributing = animator->get_clip_weight(i) > 0.001f;
                            if (contributing) {
                                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.45f, 1.0f), "%s",
                                                   animator->get_clip_name(i).c_str());
                            } else {
                                ImGui::TextDisabled("%s", animator->get_clip_name(i).c_str());
                            }
                            if (animator->has_clip_bone_mask(i) && ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Masked to part of the skeleton.");
                            }

                            ImGui::TableSetColumnIndex(1);
                            float weight = animator->get_clip_weight(i);
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            if (ImGui::SliderFloat("##w", &weight, 0.0f, 1.0f, "%.2f")) {
                                animator->set_clip_weight(i, weight);
                            }

                            ImGui::TableSetColumnIndex(2);
                            int layer = animator->get_clip_layer(i);
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            if (ImGui::DragInt("##l", &layer, 0.1f, 0, 8)) {
                                animator->set_clip_layer(i, layer);
                            }

                            ImGui::TableSetColumnIndex(3);
                            bool additive = animator->get_clip_blend_mode(i) ==
                                            AnimationPlayer::BlendMode::Additive;
                            if (ImGui::Checkbox("##a", &additive)) {
                                animator->set_clip_blend_mode(i, additive
                                    ? AnimationPlayer::BlendMode::Additive
                                    : AnimationPlayer::BlendMode::Blend);
                            }

                            ImGui::TableSetColumnIndex(4);
                            if (ImGui::SmallButton("Fade To")) {
                                animator->crossfade(i, animation_fade_seconds, animator->get_clip_looping(i));
                            }

                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }

                // --- Foot placement ---------------------------------------
                // Backed by the two-bone solver and the ground trace, so each of
                // these changes what the legs actually do.
                ImGui::Dummy(ImVec2(0.0f, 6.0f));
                if (ImGui::CollapsingHeader("Foot Placement")) {
                    auto& foot = animator->foot_placement();

                    ImGui::Checkbox("Enabled", &foot.enabled);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Traces the ground under each foot and adjusts the\n"
                                          "legs onto it, so a character stands on a slope or a\n"
                                          "stair instead of through it.");
                    }
                    if (!animator->has_ground_probe()) {
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f),
                                           "No ground probe attached.");
                    }

                    if (!foot.enabled) ImGui::BeginDisabled();

                    ImGui::DragFloat("Trace Up", &foot.trace_up, 0.01f, 0.0f, 3.0f, "%.3f m");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("How far above the animated foot the trace starts.\n"
                                          "Has to clear the tallest stair riser the character\n"
                                          "will walk up.");
                    }
                    ImGui::DragFloat("Trace Down", &foot.trace_down, 0.01f, 0.0f, 5.0f, "%.3f m");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("How far a foot will reach for ground that has\n"
                                          "dropped away below it.");
                    }
                    ImGui::DragFloat("Foot Height", &foot.foot_height, 0.005f, 0.0f, 0.5f, "%.3f m");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Distance from the foot bone to the sole. At zero the\n"
                                          "bone origin - an ankle on most rigs - sinks into\n"
                                          "the floor.");
                    }
                    ImGui::DragFloat("Max Pelvis Drop", &foot.max_pelvis_drop, 0.01f, 0.0f, 2.0f, "%.3f m");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("The pelvis only drops, never rises, and never further\n"
                                          "than this. Unbounded, one bad trace puts the\n"
                                          "character sitting on the floor.");
                    }
                    ImGui::DragFloat("Adjust Half-Life", &foot.adjust_half_life, 0.005f, 0.0f, 1.0f, "%.3f s");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Smoothing on the pelvis drop and each foot offset.\n"
                                          "Zero is instant, and pops on every stair edge.");
                    }

                    ImGui::Checkbox("Align To Normal", &foot.align_to_normal);
                    if (!foot.align_to_normal) ImGui::BeginDisabled();
                    ImGui::SliderFloat("Max Align", &foot.max_align_degrees, 0.0f, 90.0f, "%.0f deg");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Caps the tilt, so a foot cannot lie flat against a\n"
                                          "wall the trace happened to catch.");
                    }
                    if (!foot.align_to_normal) ImGui::EndDisabled();

                    ImGui::TextDisabled("Pelvis offset: %.3f m", animator->get_pelvis_offset());

                    ImGui::Dummy(ImVec2(0.0f, 4.0f));
                    if (ImGui::Button("Reset Foot Placement", ImVec2(-FLT_MIN, 0.0f))) {
                        foot.trace_up = 0.5f;
                        foot.trace_down = 0.8f;
                        foot.foot_height = 0.02f;
                        foot.max_pelvis_drop = 0.5f;
                        foot.adjust_half_life = 0.08f;
                        foot.align_to_normal = true;
                        foot.max_align_degrees = 45.0f;
                    }

                    if (!foot.enabled) ImGui::EndDisabled();
                }

                // --- Inverse kinematics -----------------------------------
                ImGui::Dummy(ImVec2(0.0f, 6.0f));
                if (ImGui::CollapsingHeader("Inverse Kinematics")) {
                    const int ik_count = animator->get_ik_count();
                    if (ik_count == 0) {
                        ImGui::TextDisabled("No IK chains on this skeleton.");
                    }
                    for (int handle = 0; handle < ik_count; ++handle) {
                        ImGui::PushID(handle);

                        int ik_root = -1, ik_mid = -1, ik_end = -1;
                        animator->get_ik_bones(handle, ik_root, ik_mid, ik_end);

                        bool ik_on = animator->is_ik_enabled(handle);
                        if (ImGui::Checkbox("##ik_enabled", &ik_on)) {
                            animator->set_ik_enabled(handle, ik_on);
                        }
                        ImGui::SameLine();
                        ImGui::Text("Chain %d", handle);
                        ImGui::SameLine();
                        // The three bones it resolved to, so a chain pointed at the
                        // wrong bone is visible rather than just not working.
                        ImGui::TextDisabled("(bones %d / %d / %d)", ik_root, ik_mid, ik_end);

                        if (!ik_on) ImGui::BeginDisabled();
                        float ik_weight = animator->get_ik_weight(handle);
                        if (ImGui::SliderFloat("Weight", &ik_weight, 0.0f, 1.0f, "%.2f")) {
                            animator->set_ik_weight(handle, ik_weight);
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Blends between the animated pose at 0 and the\n"
                                              "fully solved one at 1.");
                        }
                        if (!ik_on) ImGui::EndDisabled();

                        ImGui::Separator();
                        ImGui::PopID();
                    }
                }
            }
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 20.0f));
    ImGui::Separator();
    if (ImGui::Button("Add Attribute...", ImVec2(ImGui::GetContentRegionAvail().x, 30.0f))) {
        ImGui::OpenPopup("AddAttributePopup");
    }

    if (ImGui::BeginPopup("AddAttributePopup")) {
        if (ImGui::MenuItem("Physics Attribute")) {
            // Check if it already has one
            if (!primary_actor->get_component<PhysicsAttribute>()) {
                primary_actor->create_component<PhysicsAttribute>("Physics");
            }
        }
        if (ImGui::MenuItem("Character Controller")) {
            if (!primary_actor->get_component<CharacterControllerComponent>()) {
                primary_actor->create_component<CharacterControllerComponent>("CharacterController");
            }
        }
        if (ImGui::MenuItem("Particle Emitter")) {
            if (!primary_actor->get_component<ParticleEmitterComponent>()) {
                primary_actor->create_component<ParticleEmitterComponent>("Particles");
            }
        }
        if (ImGui::MenuItem("Terrain")) {
            if (!primary_actor->get_component<TerrainComponent>()) {
                primary_actor->create_component<TerrainComponent>("Terrain");
            }
        }
        if (ImGui::MenuItem("Nav Agent")) {
            if (!primary_actor->get_component<NavAgentComponent>()) {
                primary_actor->create_component<NavAgentComponent>("Nav Agent");
            }
        }
        if (ImGui::MenuItem("LOD Group")) {
            if (!primary_actor->get_component<LODGroupComponent>()) {
                primary_actor->create_component<LODGroupComponent>("LOD Group");
            }
        }
        if (ImGui::MenuItem("UI Canvas")) {
            // A second canvas on the same actor is legitimate - a HUD and a pause
            // menu want different sort orders and to be shown independently.
            primary_actor->create_component<UICanvasComponent>("UI Canvas");
        }
        if (ImGui::MenuItem("Joint")) {
            // Unlike the others this does not refuse a second one: a body in a
            // ragdoll or a linkage is genuinely constrained to more than one thing.
            primary_actor->create_component<JointComponent>("Joint");
        }
        if (ImGui::MenuItem("Audio Component")) {
            if (!primary_actor->get_component<AudioComponent>()) {
                primary_actor->create_component<AudioComponent>("Audio");
            }
        }
        if (ImGui::MenuItem("Lua Script Component")) {
            if (!primary_actor->get_component<LuaScriptComponent>()) {
                primary_actor->create_component<LuaScriptComponent>("Script", "");
            }
        }
        if (ImGui::MenuItem("C++ Script Component")) {
            if (!primary_actor->get_component<CppScriptComponent>()) {
                primary_actor->create_component<CppScriptComponent>("CppScript");
            }
        }
        ImGui::EndPopup();
    }
}

void Editor::draw_viewport(std::vector<std::shared_ptr<Actor>>& actors, unsigned int texture_id, bool& out_clicked, float& out_ndc_x, float& out_ndc_y, const Matrix4x4& view, const Matrix4x4& proj, Renderer* renderer) {
    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "3D Viewport");
    ImGui::Separator();
    
    // Fill the child window space
    ImVec2 viewport_size = ImGui::GetContentRegionAvail();
    ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
    
    if (renderer) {
        renderer->request_fbo_resize((int)viewport_size.x, (int)viewport_size.y);
    }
    
    // Recorded for the game UI, which has to lay itself out inside the same
    // rectangle the rendered scene occupies.
    viewport_screen_x = cursor_pos.x;
    viewport_screen_y = cursor_pos.y;
    viewport_screen_w = viewport_size.x;
    viewport_screen_h = viewport_size.y;

    // Maintain texture coordinates to prevent flipping
    ImGui::Image((void*)(intptr_t)texture_id, viewport_size, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

    // Navigation overlay, over the finished image and inside the same rectangle.
    draw_navmesh_overlay(view, proj);

    // Terrain sculpting. Applied while the mouse is held over the image, not only on
    // a click: a brush is a stroke, not a single event. The gizmo is suppressed
    // below while a brush is selected, because dragging then means painting.
    if (terrain_brush != TerrainBrush_None && ImGui::IsItemHovered() &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left) && viewport_size.x > 1.0f && viewport_size.y > 1.0f) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const float u = (mouse.x - cursor_pos.x) / viewport_size.x;
        const float v = (mouse.y - cursor_pos.y) / viewport_size.y;
        if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            const float aspect = viewport_size.y > 0.0f ? (viewport_size.x / viewport_size.y) : 1.0f;
            apply_terrain_brush(actors, view, u * 2.0f - 1.0f, 1.0f - v * 2.0f, aspect);
        }
    }

    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(cursor_pos.x, cursor_pos.y, viewport_size.x, viewport_size.y);

    if (!selected_actors.empty() && terrain_brush == TerrainBrush_None) {
        Actor* primary_actor = selected_actors[0];

        // The view matrix is rotation-only under large-world-coordinates: per-actor
        // translation is folded into get_relative_matrix against the camera origin.
        // Handing ImGuizmo an absolute world matrix against that view therefore drew
        // the gizmo offset by the entire camera position (it appeared floating off in
        // the sky rather than on the object), so build the same camera-relative matrix
        // the renderer uses and convert back afterwards.
        DVector3 lwc_origin = renderer ? renderer->get_camera_pos() : DVector3{0.0, 0.0, 0.0};
        Matrix4x4 model = primary_actor->get_actor_transform().get_relative_matrix(lwc_origin);

        ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
        if (gizmo_mode == 1) op = ImGuizmo::ROTATE;
        if (gizmo_mode == 2) op = ImGuizmo::SCALE;
        
        ImGuizmo::MODE mode = gizmo_local_space ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

        // Kept so the frame's change can be expressed as a world-space delta and
        // replayed onto the rest of the selection.
        Matrix4x4 model_before = model;

        if (ImGuizmo::Manipulate(view.m.data(), proj.m.data(), op, mode, model.m.data())) {
            // Decomposed with the engine's own inverse of get_relative_matrix(), not
            // ImGuizmo::DecomposeMatrixToComponents. That returns Euler angles in
            // ImGuizmo's order while Transform recomposes as rotZ * rotX * rotY, so
            // it round-tripped rotation incorrectly - which is why the rotate gizmo
            // mangled any actor that already had a non-zero rotation.
            Transform decomposed = Transform::from_relative_matrix(model, lwc_origin);
            Transform& t = primary_actor->get_actor_transform();

            // Still write back only the channel the gizmo is actually driving, so the
            // other two keep their exact authored values instead of drifting through a
            // decompose/recompose every frame of a drag.
            if (op == ImGuizmo::TRANSLATE) {
                t.position = decomposed.position;
            } else if (op == ImGuizmo::SCALE) {
                t.scale = decomposed.scale;
            } else {
                t.rotation = decomposed.rotation;
            }

            // Drive the rest of the selection from the same world-space delta, so a
            // multi-actor drag moves the whole group instead of only the one the
            // gizmo happens to be attached to.
            //
            // D = after * inverse(before), applied as D * M to each other actor. Going
            // through matrices rather than per-channel offsets is what makes rotate
            // and scale orbit the group about the gizmo's pivot, rather than each
            // actor spinning in place.
            if (selected_actors.size() > 1) {
                Matrix4x4 delta = model * model_before.inverse();
                for (size_t i = 1; i < selected_actors.size(); ++i) {
                    Actor* other = selected_actors[i];
                    if (!other) continue;
                    Matrix4x4 other_m = other->get_actor_transform().get_relative_matrix(lwc_origin);
                    other->get_actor_transform() =
                        Transform::from_relative_matrix(delta * other_m, lwc_origin);
                }
            }
            scene_dirty = true;
        }
        
        if (ImGuizmo::IsUsing()) {
            if (!is_dragging) {
                is_dragging = true;
                drag_start_states.clear();
                for (Actor* actor : selected_actors) {
                    drag_start_states.push_back({actor, actor->get_actor_transform()});
                }
            }
        } else if (is_dragging) {
            // Check if dragging was stopped on this frame but was active previously by gizmo
            // Since `is_dragging` is shared with properties panel, we only want to commit if ImGuizmo WAS using it.
            // Wait, this else block executes continuously when not using. We need to commit ONLY on release.
            // Let's use `ImGui::IsMouseReleased(ImGuiMouseButton_Left)` combined with `is_dragging`
            // Wait, we don't know if the dragging was started by Gizmo or Property panel if we just check `!IsUsing()`.
        }
    }
    
    // Accept drop over viewport
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_FILE")) {
            const char* path = (const char*)payload->Data;
            std::string filepath(path);
            if (is_prefab_file(filepath)) {
                instantiate_prefab(actors, filepath);
            } else if (filepath.find(".material") != std::string::npos) {
                // Apply to selected actors
                if (!selected_actors.empty()) {
                    auto dragged_mat = std::make_shared<Material>();
                    if (dragged_mat->load_from_file(filepath)) {
                        for (Actor* a : selected_actors) {
                            a->material_path = filepath;
                            a->assigned_material = dragged_mat;
                            // Update actor's visual representation immediately
                            a->actor_color = dragged_mat->albedo;
                            a->metallic = dragged_mat->metallic;
                            a->roughness = dragged_mat->roughness;
                            a->clearcoat = dragged_mat->clearcoat;
                            a->clearcoat_roughness = dragged_mat->clearcoat_roughness;
                            a->sheen = dragged_mat->sheen;
                            a->subsurface = dragged_mat->subsurface;
                        }
                    }
                }
            } else if (is_model_file(filepath)) {
                // Drop a model into the viewport to import and place it. Previously
                // adding a mesh meant editing C++ and rebuilding.
                spawn_model_actor(actors, filepath);
            } else if (filepath.find(".wav") != std::string::npos || filepath.find(".mp3") != std::string::npos || filepath.find(".ogg") != std::string::npos || filepath.find(".flac") != std::string::npos) {
                // Spawn a new audio actor!
                std::string act_name = std::filesystem::path(filepath).stem().string() + "_Audio";
                auto new_actor = std::make_unique<Actor>(act_name);
                auto audio_comp = new_actor->create_component<AudioComponent>("Audio");
                audio_comp->set_file_path(filepath);
                
                // Spawn at origin for simplicity
                new_actor->get_actor_transform().position = {0, 0, 0};
                actors.push_back(std::move(new_actor));
            } else if (filepath.find(".mesh") != std::string::npos) {
                // Spawn a new actor with this mesh!
                std::string act_name = std::filesystem::path(filepath).stem().string() + "_" + std::to_string(spawn_count++);
                auto new_actor = std::make_unique<Actor>(act_name);
                new_actor->shape_type = "StaticMesh";
                new_actor->mesh_path = filepath;
                auto mesh_comp = new_actor->create_component<StaticMeshComponent>("Mesh");
                
                auto mesh_res = ResourceManager::get().load_async<MeshResource>(filepath);
                mesh_comp->set_mesh_resource(mesh_res);
                new_actor->set_root_component(mesh_comp);
                
                // Setup initial position in front of viewport camera
                new_actor->get_actor_transform().position = { 0.0f, 0.0f, -4.5f };
                new_actor->begin_play();
                
                clear_selection();
                Actor* spawned_raw = new_actor.get();
                select_actor(spawned_raw);
        scene_dirty = true;
                
                actors.push_back(std::move(new_actor));
                record_scene_addition({ spawned_raw });
            }
        }
        ImGui::EndDragDropTarget();
    }
    
    // We handle the undo logic globally for both property window and gizmo by checking mouse release if dragging.
    if (is_dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        is_dragging = false;
        std::vector<TransformCommand::ActorTransformState> drag_end_states;
        for (Actor* actor : selected_actors) {
            drag_end_states.push_back({actor, actor->get_actor_transform()});
        }
        redo_stack.clear();
        undo_stack.push_back(std::make_unique<TransformCommand>(drag_start_states, drag_end_states));
        scene_dirty = true;
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver()) {
        ImVec2 mouse_pos = ImGui::GetMousePos();
        float local_x = mouse_pos.x - cursor_pos.x;
        float local_y = mouse_pos.y - cursor_pos.y;
        
        out_clicked = true;
        out_ndc_x = (local_x / viewport_size.x) * 2.0f - 1.0f;
        out_ndc_y = 1.0f - (local_y / viewport_size.y) * 2.0f; // Y is inverted
    } else {
        out_clicked = false;
    }
}

void Editor::draw_content_browser(Renderer* renderer) {
    // Ensure root Content/ folder always exists
    if (!std::filesystem::exists("Content")) {
        std::filesystem::create_directory("Content");
        content_browser_path = "Content";
    }
    // Safety: if current path was deleted, go back to root
    if (!std::filesystem::exists(content_browser_path)) {
        content_browser_path = "Content";
        selected_content_file = "";
    }

    // ---- Header ----
    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Content Browser");
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(4.0f, 0.0f));
    ImGui::SameLine();

    // ---- Breadcrumb navigation bar ----
    // Split content_browser_path into segments relative to Content/
    std::filesystem::path cur_path(content_browser_path);
    std::vector<std::filesystem::path> crumbs;
    std::filesystem::path tmp = cur_path;
    while (true) {
        crumbs.insert(crumbs.begin(), tmp);
        if (tmp.string() == "Content" || tmp == tmp.parent_path()) break;
        tmp = tmp.parent_path();
    }

    for (size_t i = 0; i < crumbs.size(); ++i) {
        std::string crumb_name = crumbs[i].filename().string();
        if (crumb_name.empty()) crumb_name = crumbs[i].string(); // root
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
        if (ImGui::SmallButton(crumb_name.c_str())) {
            content_browser_path = crumbs[i].string();
            selected_content_file = "";
        }
        ImGui::PopStyleColor(2);
        if (i + 1 < crumbs.size()) {
            ImGui::SameLine();
            ImGui::TextDisabled("/");
            ImGui::SameLine();
        }
    }

    // Back button (only if not at Content root)
    if (content_browser_path != "Content") {
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(8.0f, 0.0f));
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
        if (ImGui::SmallButton("  <- Back  ")) {
            content_browser_path = std::filesystem::path(content_browser_path).parent_path().string();
            selected_content_file = "";
        }
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    // ---- Grid of items ----
    const float item_w = 130.0f;
    const float item_h = 65.0f;
    ImGui::Columns(4, "ContentColumns", false);

    bool has_entries = false;

    // Helper: draw a coloured icon-style label
    auto draw_icon_button = [&](const char* icon, const char* label, bool is_selected, float w, float h) -> bool {
        if (is_selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.85f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.60f, 0.90f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.28f, 0.35f, 1.0f));
        }
        std::string combined = std::string(icon) + "\n" + label;
        bool clicked = ImGui::Button(combined.c_str(), ImVec2(w, h));
        ImGui::PopStyleColor(2);
        return clicked;
    };

    // --- Folders first ---
    for (const auto& entry : std::filesystem::directory_iterator(content_browser_path)) {
        if (!entry.is_directory()) continue;
        has_entries = true;

        std::string folder_name = entry.path().filename().string();
        std::string folder_path = entry.path().string();
        bool is_selected = (selected_content_file == folder_path);

        std::string display = folder_name.size() > 14 ? folder_name.substr(0, 12) + ".." : folder_name;
        draw_icon_button("[DIR]", display.c_str(), is_selected, item_w, item_h);

        // Single click = select
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
            selected_content_file = folder_path;
        }
        // Double click = navigate into
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            content_browser_path = folder_path;
            selected_content_file = "";
        }

        // Right-click context menu for folders
        if (ImGui::BeginPopupContextItem(("ctx_" + folder_path).c_str())) {
            selected_content_file = folder_path;
            if (ImGui::Selectable("Open")) {
                content_browser_path = folder_path;
                selected_content_file = "";
            }
            if (ImGui::Selectable("Rename")) {
                rename_mode = true;
                strncpy(rename_buf, folder_name.c_str(), sizeof(rename_buf));
                rename_buf[sizeof(rename_buf) - 1] = '\0';
            }
            if (ImGui::Selectable("Delete Folder")) {
                std::error_code ec;
                std::filesystem::remove_all(folder_path, ec);
                if (selected_content_file == folder_path) selected_content_file = "";
            }
            ImGui::EndPopup();
        }

        ImGui::NextColumn();
    }

    // --- Files ---
    for (const auto& entry : std::filesystem::directory_iterator(content_browser_path)) {
        if (!entry.is_regular_file()) continue;
        has_entries = true;

        std::string filename = entry.path().filename().string();
        std::string filepath = entry.path().string();
        std::string ext      = entry.path().extension().string();
        bool is_selected     = (selected_content_file == filepath);

        // Choose icon based on extension
        const char* icon = "[FILE]";
        if (ext == ".material") icon = "[MAT]";
        else if (ext == ".cminus") icon = "[SCR]";
        else if (ext == ".lua") icon = "[LUA]";
        else if (ext == ".vshader") icon = "[VSH]";
        else if (ext == ".mesh" || ext == ".skinnedmesh") icon = "[MSH]";
        else if (ext == ".prefab") icon = "[PFB]";
        else if (ext == ".hdr")  icon = "[HDR]";
        else if (ext == ".lithium") icon = "[SCN]";

        std::string display = filename.size() > 14 ? filename.substr(0, 12) + ".." : filename;
        draw_icon_button(icon, display.c_str(), is_selected, item_w, item_h);

        // Drag-drop source
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("CONTENT_FILE", filepath.c_str(), filepath.size() + 1);
            ImGui::Text("%s", filename.c_str());
            ImGui::EndDragDropSource();
        }

        // Single click = select
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
            selected_content_file = filepath;
        }
        // Double click = open
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (ext == ".cminus" || ext == ".lua" || ext == ".vshader") {
                open_script_file(filepath);
            } else if (ext == ".material") {
                editing_file_path = filepath;
                show_material_editor = true;
                if (active_material) active_material->load_from_file(filepath);
            } else if (ext == ".prefab") {
                if (active_actors) instantiate_prefab(*active_actors, filepath);
            }
        }

        // Right-click context menu for files
        if (ImGui::BeginPopupContextItem(("ctx_" + filepath).c_str())) {
            selected_content_file = filepath;
            if (ImGui::Selectable("Open")) {
                if (ext == ".cminus" || ext == ".lua" || ext == ".vshader") {
                    open_script_file(filepath);
                } else if (ext == ".material") {
                    editing_file_path = filepath;
                    show_material_editor = true;
                    if (active_material) active_material->load_from_file(filepath);
                }
            }
            // Always offered, whichever editor is preferred: someone using the
            // built-in editor still occasionally wants the real one, and vice versa.
            if (ext == ".cminus" || ext == ".lua" || ext == ".vshader" ||
                ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".material") {
                if (ImGui::Selectable("Open in External Editor")) {
                    // With the built-in editor preferred there is no external one
                    // configured, so this falls back to the desktop's handler.
                    const int index = (external_editor_index == ExternalEditor::kBuiltIn)
                        ? ExternalEditor::kSystemDefault
                        : external_editor_index;
                    ExternalEditor::open_file(index, external_editor_command, filepath, 0);
                }
            }
            if (ImGui::Selectable("Rename")) {
                rename_mode = true;
                strncpy(rename_buf, filename.c_str(), sizeof(rename_buf));
                rename_buf[sizeof(rename_buf) - 1] = '\0';
            }
            if (ImGui::Selectable("Duplicate")) {
                // The number goes before the extension, not after it: "Rock (1).mesh"
                // is still a mesh, "Rock.mesh (1)" is a file nothing will open.
                const std::filesystem::path source(filepath);
                const std::string stem = source.stem().string();
                const std::string extension = source.extension().string();
                const std::filesystem::path directory = source.parent_path();

                const std::string chosen = unique_name(stem,
                    [&directory, &extension](const std::string& candidate) {
                        std::error_code probe;
                        return std::filesystem::exists(directory / (candidate + extension), probe);
                    });

                std::error_code ec;
                std::filesystem::copy_file(source, directory / (chosen + extension), ec);
                if (ec) {
                    std::cerr << "[Editor] Could not duplicate " << filepath << ": "
                              << ec.message() << std::endl;
                }
            }
            if (ImGui::Selectable("Delete")) {
                std::filesystem::remove(filepath);
                if (selected_content_file == filepath) selected_content_file = "";
            }
            ImGui::EndPopup();
        }

        ImGui::NextColumn();
    }

    ImGui::Columns(1);

    if (!has_entries) {
        ImGui::TextDisabled("This folder is empty.\nRight-click for options.");
    }

    // ---- Keyboard shortcuts ----
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !selected_content_file.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
            rename_mode = true;
            std::string fname = std::filesystem::path(selected_content_file).filename().string();
            strncpy(rename_buf, fname.c_str(), sizeof(rename_buf));
            rename_buf[sizeof(rename_buf) - 1] = '\0';
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            std::error_code ec;
            if (std::filesystem::is_directory(selected_content_file))
                std::filesystem::remove_all(selected_content_file, ec);
            else
                std::filesystem::remove(selected_content_file, ec);
            selected_content_file = "";
        }
    }

    // ---- Background right-click (empty area) ----
    if (ImGui::BeginPopupContextWindow("ContentBrowserContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::Selectable("New Folder...")) {
            new_folder_mode = true;
            strncpy(new_folder_buf, "NewFolder", sizeof(new_folder_buf));
        }
        ImGui::Separator();
        if (ImGui::Selectable("Import 3D Model...")) {
            auto selection = pfd::open_file("Select a 3D Model", ".",
                                            { "3D Models", "*.obj *.fbx *.gltf *.glb *.dae *.blend",
                                              "All Files", "*" },
                                            pfd::opt::none).result();
            if (!selection.empty()) {
                ModelImporter::import_model(selection[0]);
            }
        }
        if (ImGui::Selectable("Import HDRI Environment Map...")) {
            auto selection = pfd::open_file("Select an HDRI", ".",
                                            { "HDR Images", "*.hdr", "All Files", "*" },
                                            pfd::opt::none).result();
            if (!selection.empty()) {
                std::filesystem::create_directories(content_browser_path + "/HDRI");
                std::filesystem::path dest = std::filesystem::path(content_browser_path + "/HDRI") /
                                             std::filesystem::path(selection[0]).filename();
                std::error_code ec;
                std::filesystem::copy_file(selection[0], dest, std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec && renderer) renderer->load_environment_map(dest.string());
            }
        }
        ImGui::Separator();
        if (ImGui::Selectable("Create Lua Script (.lua)")) {
            std::string path = content_browser_path + "/NewScript_" + std::to_string(std::time(nullptr)) + ".lua";
            std::ofstream file(path);
            file << "-- Lua gameplay script\n"
                    "-- Reloads automatically when you save.\n\n"
                    "function on_begin_play()\n"
                    "    log(\"hello from\", actor.get_name())\n"
                    "end\n\n"
                    "function on_tick(dt)\n"
                    "    actor.rotate(0.0, dt, 0.0)\n"
                    "end\n";
        }
        if (ImGui::Selectable("Create C-Minus Script (.cminus)")) {
            std::string path = content_browser_path + "/NewScript_" + std::to_string(std::time(nullptr)) + ".cminus";
            std::ofstream file(path);
            file << "// C-Minus Script\n// Try: set_position(0.0, 1.0, 0.0)\n";
        }
        if (ImGui::Selectable("Create Visual Shader (.vshader)")) {
            std::string path = content_browser_path + "/NewShader_" + std::to_string(std::time(nullptr)) + ".vshader";
            std::ofstream file(path);
            file << "Visual Shader Template\n";
        }
        if (ImGui::Selectable("Create Material (.material)")) {
            std::string path = content_browser_path + "/NewMaterial_" + std::to_string(std::time(nullptr)) + ".material";
            Material new_mat;
            new_mat.save_to_file(path);
        }
        ImGui::EndPopup();
    }

    // ---- New Folder modal ----
    if (new_folder_mode) {
        ImGui::OpenPopup("New Folder");
        new_folder_mode = false;
    }
    if (ImGui::BeginPopupModal("New Folder", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Folder name:");
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool confirmed = ImGui::InputText("##newfoldername", new_folder_buf, sizeof(new_folder_buf),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        if (ImGui::Button("Create", ImVec2(130, 0)) || confirmed) {
            std::string folder_name(new_folder_buf);
            // Sanitise: strip any path separators
            folder_name.erase(std::remove_if(folder_name.begin(), folder_name.end(),
                [](char c){ return c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|'; }),
                folder_name.end());
            if (!folder_name.empty()) {
                std::string new_path = content_browser_path + "/" + folder_name;
                std::filesystem::create_directory(new_path);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(130, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Rename modal (files AND folders) ----
    if (rename_mode && !selected_content_file.empty()) {
        ImGui::OpenPopup("Rename");
        rename_mode = false;
    }
    if (ImGui::BeginPopupModal("Rename", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("New name:");
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool confirmed = ImGui::InputText("##renamefield", rename_buf, sizeof(rename_buf),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        if (ImGui::Button("OK", ImVec2(140, 0)) || confirmed) {
            std::string new_name(rename_buf);
            // Sanitise
            new_name.erase(std::remove_if(new_name.begin(), new_name.end(),
                [](char c){ return c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|'; }),
                new_name.end());
            if (!new_name.empty()) {
                std::filesystem::path p(selected_content_file);
                const std::filesystem::path directory = p.parent_path();

                // Renaming onto an existing file would overwrite it silently and
                // destroy an asset, so the same " (N)" convention used for
                // duplicates resolves the collision instead.
                const std::filesystem::path wanted(new_name);
                const std::string stem = wanted.stem().string();
                const std::string extension = wanted.extension().string();
                const std::string chosen = unique_name(stem,
                    [&directory, &extension, this](const std::string& candidate) {
                        const std::filesystem::path probe_path =
                            directory / (candidate + extension);
                        // The file's own current name is not a collision.
                        if (probe_path.string() == selected_content_file) return false;
                        std::error_code probe;
                        return std::filesystem::exists(probe_path, probe);
                    });

                const std::string new_path = (directory / (chosen + extension)).string();
                if (new_path != selected_content_file) {
                    std::error_code ec;
                    std::filesystem::rename(selected_content_file, new_path, ec);
                    if (!ec) {
                        if (editing_file_path == selected_content_file) editing_file_path = new_path;
                        selected_content_file = new_path;
                    } else {
                        std::cerr << "[Editor] Could not rename " << selected_content_file
                                  << ": " << ec.message() << std::endl;
                    }
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(140, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Editor::draw_script_editor() {
    ImGui::Text("Editing: %s", editing_file_path.c_str());
    if (ImGui::Button("Save")) {
        std::ofstream file(editing_file_path);
        if (file.is_open()) {
            file << editing_file_content;
            file.close();
        }
    }
    static std::string compile_msg = "";
    static ImVec4 compile_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    if (ImGui::Button("Check for errors")) {
        std::string ext = std::filesystem::path(editing_file_path).extension().string();
        if (ext == ".lua") {
            std::string error;
            if (LuaAPI::check_syntax(editing_file_content, std::filesystem::path(editing_file_path).filename().string(), error)) {
                compile_msg = "No syntax errors!";
                compile_color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
            } else {
                compile_msg = error;
                compile_color = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
            }
        } else if (ext == ".cminus") {
            try {
                CMinus::Lexer lexer(editing_file_content);
                auto tokens = lexer.tokenize();
                CMinus::Parser parser(tokens);
                parser.parse();
                std::cout << "No syntax errors found in " << editing_file_path << std::endl;
                compile_msg = "No syntax errors!";
                compile_color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
            } catch (const std::exception& e) {
                std::cerr << "Syntax Error: " << e.what() << std::endl;
                compile_msg = std::string("Syntax Error: ") + e.what();
                compile_color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
            }
        }
    }
    
    if (!compile_msg.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(compile_color, "%s", compile_msg.c_str());
    }
    ImGui::Separator();
    
    if (ImGui::Button("Save Script")) {
        std::ofstream file(editing_file_path);
        file << editing_file_content;
        file.close();
    }
    ImGui::SameLine();
    ImGui::Text("Editing: %s", std::filesystem::path(editing_file_path).filename().string().c_str());
    ImGui::Separator();

    static char script_buffer[1024 * 64];
    strncpy(script_buffer, editing_file_content.c_str(), sizeof(script_buffer) - 1);
    
    ImGui::InputTextMultiline("##source", script_buffer, sizeof(script_buffer), 
                              ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_AllowTabInput);
                              
    editing_file_content = script_buffer;
}

void Editor::draw_material_editor() {
    if (editing_file_path.empty() || !active_material) return;
    
    if (ImGui::Button("Save Material")) {
        active_material->save_to_file(editing_file_path);
        // Refresh all actors using this material
        // In a real engine, we'd fire an event or have a material manager. For now, since actors 
        // store a reference, the properties are updated in `active_material` but their `actor_color` won't update
        // unless we force it. Wait, if `Tesla` uses `assigned_material` directly, it will update!
    }
    ImGui::SameLine();
    ImGui::Text("Editing: %s", std::filesystem::path(editing_file_path).filename().string().c_str());
    ImGui::Separator();

    ImGui::BeginChild("MaterialProperties", ImVec2(0, 0), true);

    // --- Custom surface shader ---------------------------------------------
    // A material with a shader stops using the base properties below entirely: its
    // fragment stage is the author's, and albedo/roughness/emission come from there.
    ImGui::Text("Surface Shader");
    char shader_buf[256];
    strncpy(shader_buf, active_material->shader_path.c_str(), sizeof(shader_buf));
    shader_buf[sizeof(shader_buf) - 1] = '\0';
    if (ImGui::InputText("Shader (.lshader)", shader_buf, sizeof(shader_buf))) {
        active_material->shader_path = shader_buf;
        active_material->shader_values.clear();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_FILE")) {
            std::string dropped((const char*)payload->Data);
            if (dropped.find(".lshader") != std::string::npos) {
                active_material->shader_path = dropped;
                active_material->shader_values.clear();
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (active_material->shader_path.empty()) {
        ImGui::TextDisabled("None: this material uses the engine's standard PBR shading.");
        ImGui::TextDisabled("A .lshader is two blocks - properties { } and surface { } -");
        ImGui::TextDisabled("where the surface block is GLSL writing Albedo, Roughness,");
        ImGui::TextDisabled("Metallic, Emissive and ShadingNormal.");
    } else {
        if (ImGui::Button("Compile")) {
            std::string error;
            MaterialShaderLibrary::get().reload(active_material->shader_path, error);
        }
        ImGui::SameLine();
        auto shader = MaterialShaderLibrary::get().load(active_material->shader_path);
        if (shader && shader->is_valid()) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Compiled");

            for (const MaterialShader::Property& property : shader->get_properties()) {
                ImGui::PushID(property.name.c_str());

                // The material's value if it has one, the shader's default otherwise.
                Material::ShaderValue* stored = nullptr;
                for (auto& candidate : active_material->shader_values) {
                    if (candidate.name == property.name) { stored = &candidate; break; }
                }

                Material::ShaderValue current;
                current.name = property.name;
                if (stored) {
                    current = *stored;
                } else {
                    current.number[0] = property.default_value[0];
                    current.number[1] = property.default_value[1];
                    current.number[2] = property.default_value[2];
                    current.texture_path = property.default_texture;
                }

                bool changed = false;
                if (property.type == MaterialShader::Property::Type::Float) {
                    changed = ImGui::DragFloat(property.name.c_str(), &current.number[0], 0.01f);
                } else if (property.type == MaterialShader::Property::Type::Color) {
                    changed = ImGui::ColorEdit3(property.name.c_str(), current.number);
                } else {
                    char texture_buf[256];
                    strncpy(texture_buf, current.texture_path.c_str(), sizeof(texture_buf));
                    texture_buf[sizeof(texture_buf) - 1] = '\0';
                    if (ImGui::InputText(property.name.c_str(), texture_buf, sizeof(texture_buf))) {
                        current.texture_path = texture_buf;
                        changed = true;
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_FILE")) {
                            current.texture_path = std::string((const char*)payload->Data);
                            changed = true;
                        }
                        ImGui::EndDragDropTarget();
                    }
                }

                if (changed) {
                    if (stored) *stored = current;
                    else active_material->shader_values.push_back(current);
                }
                ImGui::PopID();
            }
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "Failed");
            const std::string& error =
                MaterialShaderLibrary::get().last_error_for(active_material->shader_path);
            if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
            ImGui::TextDisabled("Falling back to standard shading until it compiles.");
        }
    }

    ImGui::Separator();
    ImGui::Text("Base Properties");
    ImGui::ColorEdit3("Albedo", &active_material->albedo.x);
    ImGui::SliderFloat("Metallic", &active_material->metallic, 0.0f, 1.0f);
    ImGui::SliderFloat("Roughness", &active_material->roughness, 0.0f, 1.0f);
    
    ImGui::Separator();
    ImGui::Text("Emission");
    ImGui::SliderFloat("Emission Strength", &active_material->emission, 0.0f, 100.0f);
    ImGui::ColorEdit3("Emission Color", &active_material->emission_color.x);
    
    ImGui::Separator();
    ImGui::Text("Advanced Specular");
    ImGui::SliderFloat("Specular", &active_material->specular, 0.0f, 1.0f);
    ImGui::SliderFloat("Specular Tint", &active_material->specular_tint, 0.0f, 1.0f);
    ImGui::SliderFloat("Anisotropy", &active_material->anisotropy, 0.0f, 1.0f);
    ImGui::SliderFloat("Anisotropy Rotation", &active_material->anisotropy_rotation, 0.0f, 1.0f);
    
    ImGui::Separator();
    ImGui::Text("Clearcoat & Sheen");
    ImGui::SliderFloat("Clearcoat", &active_material->clearcoat, 0.0f, 1.0f);
    ImGui::SliderFloat("Clearcoat Roughness", &active_material->clearcoat_roughness, 0.0f, 1.0f);
    ImGui::SliderFloat("Sheen", &active_material->sheen, 0.0f, 1.0f);
    ImGui::SliderFloat("Sheen Tint", &active_material->sheen_tint, 0.0f, 1.0f);
    
    ImGui::Separator();
    ImGui::Text("Transmission & Subsurface");
    ImGui::SliderFloat("Transmission", &active_material->transmission, 0.0f, 1.0f);
    ImGui::SliderFloat("IOR", &active_material->ior, 1.0f, 3.0f);
    ImGui::SliderFloat("Subsurface", &active_material->subsurface, 0.0f, 1.0f);
    ImGui::ColorEdit3("Subsurface Color", &active_material->subsurface_color.x);
    ImGui::SliderFloat("Thickness", &active_material->thickness, 0.0f, 10.0f);

    ImGui::Separator();
    ImGui::Text("Volumetric & Effects");
    ImGui::SliderFloat("Alpha / Opacity", &active_material->alpha, 0.0f, 1.0f);
    ImGui::SliderFloat("Normal Strength", &active_material->normal_strength, 0.0f, 10.0f);
    ImGui::SliderFloat("Displacement Scale", &active_material->displacement_scale, 0.0f, 1.0f);
    ImGui::SliderFloat("Fuzz", &active_material->fuzz, 0.0f, 1.0f);
    ImGui::ColorEdit3("Absorption Color", &active_material->absorption_color.x);
    ImGui::ColorEdit3("Scatter Color", &active_material->scatter_color.x);

    ImGui::EndChild();
}

// Groups digits so a triangle count reads as 1,240,318 rather than 1240318.
static std::string format_thousands(long long v) {
    std::string digits = std::to_string(v < 0 ? -v : v);
    std::string out;
    int count = 0;
    for (int i = static_cast<int>(digits.size()) - 1; i >= 0; --i) {
        out.push_back(digits[i]);
        if (++count % 3 == 0 && i > 0) out.push_back(',');
    }
    if (v < 0) out.push_back('-');
    std::reverse(out.begin(), out.end());
    return out;
}

// Per-frame cost breakdown. The point is attribution: a single FPS number says the
// frame is slow, this says which pass is responsible for it.
void Editor::draw_profiler(Renderer* renderer, const std::vector<std::shared_ptr<Actor>>& actors) {
    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Profiler");
    ImGui::Separator();

    if (!renderer) {
        ImGui::TextDisabled("Renderer not initialised.");
        return;
    }

    RenderProfiler& prof = renderer->profiler;

    bool on = prof.enabled();
    if (ImGui::Checkbox("Enabled", &on)) prof.set_enabled(on);
    ImGui::SameLine();
    ImGui::TextDisabled("(GPU timings lag a few frames - queries read back asynchronously)");

    float frame_ms = 1000.0f / std::max(1.0f, ImGui::GetIO().Framerate);
    ImGui::Text("Frame: %.2f ms  (%.1f FPS)", frame_ms, ImGui::GetIO().Framerate);
    ImGui::Separator();

    if (!on) {
        ImGui::TextDisabled("Profiling disabled.");
        return;
    }

    float total_gpu = prof.total_gpu_ms();

    if (ImGui::BeginTable("PassTable", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("CPU (ms)", ImGuiTableColumnFlags_WidthStretch, 0.16f);
        ImGui::TableSetupColumn("GPU (ms)", ImGuiTableColumnFlags_WidthStretch, 0.16f);
        ImGui::TableSetupColumn("GPU share", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < RenderProfiler::PassCount; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(RenderProfiler::pass_name(i));
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.2f", prof.cpu_ms[i]);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.2f", prof.gpu_ms[i]);
            ImGui::TableSetColumnIndex(3);
            float frac = (total_gpu > 0.0001f) ? (prof.gpu_ms[i] / total_gpu) : 0.0f;
            // Colour the worst offender so the expensive pass is findable at a glance
            // rather than by reading every number in the column.
            ImVec4 bar = (frac > 0.4f) ? ImVec4(0.85f, 0.25f, 0.20f, 1.0f)
                       : (frac > 0.2f) ? ImVec4(0.85f, 0.65f, 0.20f, 1.0f)
                                       : ImVec4(0.25f, 0.60f, 0.35f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar);
            char overlay[32];
            snprintf(overlay, sizeof(overlay), "%.0f%%", frac * 100.0f);
            ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0.0f), overlay);
            ImGui::PopStyleColor();
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Total");
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.2f", prof.total_cpu_ms());
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%.2f", total_gpu);
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Scene");
    ImGui::Separator();

    size_t component_count = 0;
    size_t light_count = 0;
    for (const auto& a : actors) {
        component_count += a->get_components().size();
        if (a->get_component<LightComponent>()) ++light_count;
    }

    ImGui::Text("Draw calls : %d", prof.draw_calls);
    ImGui::Text("Triangles  : %s", format_thousands(prof.triangles).c_str());

    // Visibility culling. Both counts are for the geometry pass only - the shadow
    // pass deliberately draws everything, because an object hidden from the camera
    // can still cast a shadow that is not.
    if (renderer) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::Checkbox("Frustum culling", &renderer->enable_frustum_culling);
        ImGui::SameLine();
        ImGui::Checkbox("Occlusion culling", &renderer->enable_occlusion_culling);
        ImGui::Text("Culled     : %d frustum, %d occluded",
                    renderer->culled_by_frustum, renderer->culled_by_occlusion);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Occlusion results come from the previous frame's GPU queries.\n"
                              "Reading them the frame they are issued would stall the pipeline.");
        }
    }
    ImGui::Text("Actors     : %zu", actors.size());
    ImGui::Text("Components : %zu", component_count);
    ImGui::Text("Lights     : %zu", light_count);
    ImGui::Text("Selected   : %zu", selected_actors.size());
}

// Rebinding UI. Actions are addressed by index from C-minus scripts (its calls carry
// only floats), so the index is shown beside each name rather than left for the user
// to count out.

// --- Navigation ------------------------------------------------------------




void Editor::draw_lighting_bake(std::vector<std::shared_ptr<Actor>>& actors) {
    if (!show_lighting_bake) return;

    ImGui::SetNextWindowSize(ImVec2(470.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Baked Lighting", &show_lighting_bake)) {
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Global Illumination");
    ImGui::Separator();
    ImGui::TextWrapped("Actors marked Static get a lightmap. Everything else is lit by "
                       "the probe grid, which is sampled wherever the object happens "
                       "to be.");

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::TextDisabled("Lighting");
    ImGui::ColorEdit3("Sky Color", &bake_settings.sky_color.x);
    ImGui::DragFloat("Sky Intensity", &bake_settings.sky_intensity, 0.05f, 0.0f, 20.0f);
    ImGui::ColorEdit3("Sun Color", &bake_settings.sun_color.x);
    ImGui::DragFloat("Sun Intensity", &bake_settings.sun_intensity, 0.05f, 0.0f, 50.0f);
    ImGui::DragFloat3("Sun Direction", &bake_settings.sun_direction.x, 0.02f, -1.0f, 1.0f);
    ImGui::TextDisabled("Direction the light travels, so downward is negative Y.");

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::TextDisabled("Quality");
    ImGui::DragInt("Atlas Size", &bake_settings.atlas_size, 16.0f, 64, 4096);
    ImGui::DragFloat("Texels Per Unit", &bake_settings.texels_per_unit, 0.25f, 0.5f, 64.0f);
    ImGui::DragInt("Rays Per Texel", &bake_settings.rays_per_texel, 1.0f, 4, 1024);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Noise falls with the square root of this, so doubling the\n"
                          "quality costs four times the time.");
    }
    ImGui::Checkbox("Bake Probes", &bake_settings.bake_probes);
    if (bake_settings.bake_probes) {
        ImGui::DragFloat("Probe Spacing", &bake_settings.probe_spacing, 0.1f, 0.5f, 64.0f, "%.1f m");
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (ImGui::Button("Bake", ImVec2(140.0f, 32.0f))) {
        // The bake blocks. Progress goes to the log rather than to a live progress
        // bar: drawing one would mean pumping the UI from inside the bake, and the
        // bake holds the scene lock.
        auto progress = [](const char* label, float fraction) {
            std::cout << "[Lightmap] " << static_cast<int>(fraction * 100.0f) << "% " << label << std::endl;
        };
        if (Lightmapper::get().bake(actors, bake_settings, progress, bake_report)) {
            Lightmapper::get().apply_to_actors(actors);
            // The old per-actor scalar bake writes into the same lighting term. Left
            // set, the two would stack and every static surface would be lit twice.
            for (auto& actor : actors) {
                if (actor) actor->has_baked_lighting = false;
            }
            scene_dirty = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(100.0f, 32.0f))) {
        Lightmapper::get().clear();
        Lightmapper::get().apply_to_actors(actors);
        bake_report = "Baked lighting cleared.";
        scene_dirty = true;
    }

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    if (Lightmapper::get().is_baked()) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.45f, 1.0f), "Baked (%dpx atlas)",
                           Lightmapper::get().get_atlas_size());
    } else {
        ImGui::TextDisabled("Nothing baked. Dynamic objects fall back to flat sky light.");
    }
    if (!bake_report.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", bake_report.c_str());
    }
    ImGui::TextDisabled("Saved beside the scene when the scene is saved.");

    ImGui::End();
}

void Editor::draw_audio_buses() {
    if (!show_audio_buses) return;

    ImGui::SetNextWindowSize(ImVec2(460.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Audio Buses", &show_audio_buses)) {
        ImGui::End();
        return;
    }

    AudioEngine& audio = AudioEngine::get();
    if (!audio.initialized()) {
        ImGui::TextDisabled("Audio is not running yet. Open a project first.");
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("Every sound plays into one bus. Effects here apply to the "
                       "whole bus, so muffling everything but the UI is one change "
                       "rather than one per sound.");
    ImGui::Separator();

    for (int bus = 0; bus < AudioEngine::kBusCount; ++bus) {
        ImGui::PushID(bus);
        auto& settings = audio.get_bus_settings(bus);

        char name_buf[64];
        strncpy(name_buf, settings.name.c_str(), sizeof(name_buf));
        name_buf[sizeof(name_buf) - 1] = '\0';
        if (ImGui::CollapsingHeader(settings.name.c_str(),
                                    bus == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            if (ImGui::InputText("Name", name_buf, sizeof(name_buf))) settings.name = name_buf;
            if (ImGui::SliderFloat("Volume", &settings.volume, 0.0f, 2.0f, "%.2f")) {
                audio.apply_bus_volume(bus);
            }

            BusEffectNode* effects = audio.get_bus_effects(bus);
            if (!effects) {
                ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1.0f),
                                   "Effect chain unavailable on this bus.");
            } else {
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
                ImGui::Checkbox("Low-pass", &effects->lowpass_enabled);
                if (effects->lowpass_enabled) {
                    ImGui::SliderFloat("Cutoff##lp", &effects->lowpass_cutoff, 60.0f, 18000.0f,
                                       "%.0f Hz", ImGuiSliderFlags_Logarithmic);
                }

                ImGui::Checkbox("High-pass", &effects->highpass_enabled);
                if (effects->highpass_enabled) {
                    ImGui::SliderFloat("Cutoff##hp", &effects->highpass_cutoff, 20.0f, 4000.0f,
                                       "%.0f Hz", ImGuiSliderFlags_Logarithmic);
                }

                ImGui::Checkbox("Delay", &effects->delay_enabled);
                if (effects->delay_enabled) {
                    ImGui::SliderFloat("Time", &effects->delay_seconds, 0.01f, 2.0f, "%.2f s");
                    ImGui::SliderFloat("Feedback", &effects->delay_feedback, 0.0f, 0.95f, "%.2f");
                    ImGui::SliderFloat("Mix##delay", &effects->delay_mix, 0.0f, 1.0f, "%.2f");
                }

                ImGui::Checkbox("Reverb", &effects->reverb_enabled);
                if (effects->reverb_enabled) {
                    ImGui::SliderFloat("Room Size", &effects->reverb_room_size, 0.0f, 1.0f, "%.2f");
                    ImGui::SliderFloat("Damping", &effects->reverb_damping, 0.0f, 1.0f, "%.2f");
                    ImGui::SliderFloat("Wet##reverb", &effects->reverb_wet, 0.0f, 1.0f, "%.2f");
                    ImGui::TextDisabled("Damping decides how fast the highs die away.");
                }
            }
        }
        ImGui::PopID();
    }

    ImGui::End();
}

void Editor::draw_collision_layers() {
    if (!show_collision_layers) return;

    ImGui::SetNextWindowSize(ImVec2(620.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Collision Layers", &show_collision_layers)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("Bodies only collide when their two layers are ticked against "
                       "each other. This is how a projectile passes through its own "
                       "shooter, or debris ignores everything but the world.");
    ImGui::TextDisabled("Applies to new bodies; press Play again to rebuild existing ones.");
    ImGui::Separator();

    if (ImGui::Button("Everything Collides")) PhysicsEngine::reset_layers();
    ImGui::SameLine();
    if (ImGui::Button("Nothing Collides")) {
        for (int a = 0; a < PhysicsEngine::kLayerCount; ++a) {
            for (int b = a; b < PhysicsEngine::kLayerCount; ++b) {
                PhysicsEngine::set_layers_collide(a, b, false);
            }
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    if (ImGui::CollapsingHeader("Layer names", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Only the layers actually in use are worth showing by default; thirty-two
        // text boxes is a wall nobody reads.
        static int visible_names = 8;
        ImGui::SliderInt("Show", &visible_names, 1, PhysicsEngine::kLayerCount);
        for (int layer = 0; layer < visible_names; ++layer) {
            ImGui::PushID(layer);
            char name_buf[64];
            strncpy(name_buf, PhysicsEngine::get_layer_name(layer).c_str(), sizeof(name_buf));
            name_buf[sizeof(name_buf) - 1] = '\0';
            char label[24];
            snprintf(label, sizeof(label), "Layer %d", layer);
            if (ImGui::InputText(label, name_buf, sizeof(name_buf))) {
                PhysicsEngine::set_layer_name(layer, name_buf);
            }
            ImGui::PopID();
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextDisabled("Matrix");
    // Triangular, because the relation is symmetric: drawing both halves would offer
    // two checkboxes for one fact and invite them to disagree.
    static int matrix_size = 8;
    ImGui::SliderInt("Layers shown", &matrix_size, 2, PhysicsEngine::kLayerCount);

    if (ImGui::BeginChild("LayerMatrix", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_HorizontalScrollbar)) {
        const float label_width = 140.0f;
        for (int row = 0; row < matrix_size; ++row) {
            ImGui::PushID(row);
            ImGui::Text("%d %s", row, PhysicsEngine::get_layer_name(row).c_str());
            ImGui::SameLine(label_width);
            for (int column = 0; column <= row; ++column) {
                ImGui::PushID(column);
                bool collides = PhysicsEngine::layers_should_collide(row, column);
                if (ImGui::Checkbox("##cell", &collides)) {
                    PhysicsEngine::set_layers_collide(row, column, collides);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s  x  %s",
                                      PhysicsEngine::get_layer_name(row).c_str(),
                                      PhysicsEngine::get_layer_name(column).c_str());
                }
                ImGui::SameLine();
                ImGui::PopID();
            }
            ImGui::NewLine();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

void Editor::draw_navigation_window(std::vector<std::shared_ptr<Actor>>& actors) {
    if (!show_navigation_window) return;

    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Navigation", &show_navigation_window)) {
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Navigation Mesh");
    ImGui::Separator();
    ImGui::TextWrapped("Built from every mesh in the scene except trigger volumes and "
                       "anything carrying a character controller. Surfaces must face "
                       "upward to be walkable.");

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::TextDisabled("Agent");
    ImGui::DragFloat("Radius", &nav_settings.agent_radius, 0.01f, 0.05f, 5.0f, "%.2f m");
    ImGui::DragFloat("Height", &nav_settings.agent_height, 0.05f, 0.2f, 10.0f, "%.2f m");
    ImGui::DragFloat("Max Slope", &nav_settings.max_slope_degrees, 0.5f, 0.0f, 89.0f, "%.0f deg");
    ImGui::DragFloat("Step Height", &nav_settings.step_height, 0.01f, 0.0f, 3.0f, "%.2f m");
    ImGui::DragFloat("Max Drop", &nav_settings.max_drop, 0.05f, 0.0f, 50.0f, "%.2f m");

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::TextDisabled("Build");
    ImGui::DragFloat("Cell Size", &nav_settings.cell_size, 0.01f, 0.05f, 4.0f, "%.2f m");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Horizontal resolution. Halving it quadruples the build time\n"
                          "and the memory the navmesh occupies.");
    }
    ImGui::Checkbox("Allow Diagonals", &nav_settings.allow_diagonals);

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (ImGui::Button("Build Navmesh", ImVec2(150.0f, 30.0f))) {
        NavMesh::get().build(actors, nav_settings, nav_build_report);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(90.0f, 30.0f))) {
        NavMesh::get().clear();
        nav_build_report = "Navmesh cleared.";
    }

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::Checkbox("Show walkable surface", &show_navmesh_overlay);

    NavMesh& navmesh = NavMesh::get();
    if (navmesh.is_built()) {
        ImGui::TextDisabled("%d surfaces, %d reachable",
                            navmesh.get_node_count(), navmesh.get_usable_node_count());
    } else {
        ImGui::TextDisabled("No navmesh built.");
    }
    if (!nav_build_report.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", nav_build_report.c_str());
    }

    ImGui::End();
}

void Editor::draw_navmesh_overlay(const Matrix4x4& view, const Matrix4x4& proj) {
    if (!show_navmesh_overlay) return;
    NavMesh& navmesh = NavMesh::get();
    if (!navmesh.is_built()) return;
    if (viewport_screen_w < 1.0f || viewport_screen_h < 1.0f) return;

    const std::vector<Vector3>& cells = navmesh.get_debug_cells();
    if (cells.empty()) return;

    // The view matrix is rotation-only under large-world-coordinates, so world
    // positions have to be made camera-relative before they are transformed - the
    // same thing the gizmo above does with get_relative_matrix.
    const DVector3 origin = active_renderer ? active_renderer->get_camera_pos() : DVector3{ 0.0, 0.0, 0.0 };
    const Matrix4x4 view_projection = proj * view;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->PushClipRect(ImVec2(viewport_screen_x, viewport_screen_y),
                            ImVec2(viewport_screen_x + viewport_screen_w,
                                   viewport_screen_y + viewport_screen_h), true);

    const float half = navmesh.get_cell_size() * 0.5f;
    // A cap on how many cells are drawn: a large level holds hundreds of thousands,
    // and this is a debug overlay, not a render path. Cells are strided rather than
    // truncated so the sample still covers the whole level.
    constexpr int kMaxDrawn = 6000;
    const int stride = std::max(1, static_cast<int>(cells.size()) / kMaxDrawn);

    auto project = [&](const Vector3& world, ImVec2& out) -> bool {
        const float rx = static_cast<float>(world.x - origin.x);
        const float ry = static_cast<float>(world.y - origin.y);
        const float rz = static_cast<float>(world.z - origin.z);
        const std::array<float, 16>& m = view_projection.m;
        const float cx = m[0] * rx + m[4] * ry + m[8]  * rz + m[12];
        const float cy = m[1] * rx + m[5] * ry + m[9]  * rz + m[13];
        const float cw = m[3] * rx + m[7] * ry + m[11] * rz + m[15];
        // Behind the camera: the divide would flip the point to the opposite side of
        // the screen and draw a quad across the whole viewport.
        if (cw <= 1e-4f) return false;
        const float ndc_x = cx / cw;
        const float ndc_y = cy / cw;
        out.x = viewport_screen_x + (ndc_x * 0.5f + 0.5f) * viewport_screen_w;
        out.y = viewport_screen_y + (1.0f - (ndc_y * 0.5f + 0.5f)) * viewport_screen_h;
        return true;
    };

    for (size_t i = 0; i < cells.size(); i += stride) {
        const Vector3& centre = cells[i];
        // Lifted a little so the quad is not z-fighting the floor it describes -
        // though nothing depth-tests here, it also keeps it visually distinct.
        const float y = centre.y + 0.02f;
        ImVec2 corners[4];
        bool visible = true;
        visible &= project({ centre.x - half, y, centre.z - half }, corners[0]);
        visible &= project({ centre.x + half, y, centre.z - half }, corners[1]);
        visible &= project({ centre.x + half, y, centre.z + half }, corners[2]);
        visible &= project({ centre.x - half, y, centre.z + half }, corners[3]);
        if (!visible) continue;

        draw_list->AddQuadFilled(corners[0], corners[1], corners[2], corners[3],
                                 IM_COL32(70, 170, 255, 60));
    }

    draw_list->PopClipRect();
}

void Editor::draw_input_bindings() {
    if (std::getenv("LITHIUM_TEST_INPUT")) show_input_bindings = true; // TEMP-VERIFY
    if (!show_input_bindings) return;

    InputMap& map = InputMap::get();

    ImGui::SetNextWindowSize(ImVec2(660.0f, 470.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Input Bindings", &show_input_bindings)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("Named actions, so gameplay code asks for \"Jump\" rather than a "
                       "hardcoded key. Click a binding to rebind it, then press a key. "
                       "Esc cancels. Scripts use the [index] shown on the left.");
    ImGui::Separator();
    ImGui::Text("Gamepad: %s", map.gamepad_name());
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan")) map.refresh_gamepad();
    ImGui::Separator();

    // Polls key state rather than reading the event queue: the engine already drains
    // events for its own shortcuts, so a second consumer here would fight it.
    if (input_capture_target >= 0) {
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        int act_i = input_capture_target / 1000;
        int bind_i = input_capture_target % 1000;
        InputAction* act = map.action_at(act_i);

        if (keys && keys[SDL_SCANCODE_ESCAPE]) {
            input_capture_target = -1;
        } else if (keys && act && bind_i < static_cast<int>(act->bindings.size())) {
            for (int sc = 0; sc < SDL_NUM_SCANCODES; ++sc) {
                if (sc == SDL_SCANCODE_ESCAPE || !keys[sc]) continue;
                act->bindings[bind_i].source = InputSource::Key;
                act->bindings[bind_i].code = sc;
                input_capture_target = -1;
                break;
            }
        } else {
            input_capture_target = -1;
        }
    }

    ImGui::BeginChild("ActionList", ImVec2(0.0f, -34.0f), ImGuiChildFlags_Borders);
    for (int i = 0; i < map.action_count(); ++i) {
        InputAction* a = map.action_at(i);
        if (!a) continue;

        ImGui::PushID(i);

        // Live value, so a binding can be confirmed by pressing the key and watching
        // this move, rather than by launching the game to find out.
        ImGui::TextColored(a->held ? ImVec4(0.35f, 0.85f, 0.40f, 1.0f) : ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                           "[%d] %s", i, a->name.c_str());
        ImGui::SameLine(230.0f);
        ImGui::ProgressBar((a->value + 1.0f) * 0.5f, ImVec2(110.0f, 0.0f), a->held ? "held" : "");
        ImGui::SameLine();
        if (ImGui::SmallButton("+ Binding")) {
            a->bindings.push_back({ InputSource::Key, SDL_SCANCODE_UNKNOWN, 1.0f });
        }
        ImGui::SameLine();
        bool remove_action = ImGui::SmallButton("Remove");

        ImGui::Indent(18.0f);
        for (int b = 0; b < static_cast<int>(a->bindings.size()); ++b) {
            ImGui::PushID(b);
            bool capturing = (input_capture_target == i * 1000 + b);
            std::string label = capturing ? "< press a key >" : InputMap::describe(a->bindings[b]);
            if (ImGui::Button(label.c_str(), ImVec2(200.0f, 0.0f))) {
                input_capture_target = capturing ? -1 : (i * 1000 + b);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Scale -1 makes this binding drive the action negatively,\n"
                                  "which is how one action carries both W and S.");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            ImGui::DragFloat("scale", &a->bindings[b].scale, 0.05f, -1.0f, 1.0f, "%.2f");
            ImGui::SameLine();
            bool drop = ImGui::SmallButton("x");
            ImGui::PopID();
            if (drop) {
                a->bindings.erase(a->bindings.begin() + b);
                break;
            }
        }
        ImGui::Unindent(18.0f);
        ImGui::Separator();
        ImGui::PopID();

        // Done after PopID so the erase cannot invalidate `a` while it is still in use.
        if (remove_action) {
            map.remove_action(i);
            break;
        }
    }
    ImGui::EndChild();

    if (ImGui::Button("Add Action")) map.add_action("NewAction");
    ImGui::SameLine();
    if (ImGui::Button("Save")) map.save("input_bindings.json");
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        if (!map.load("input_bindings.json")) map.load_defaults();
    }
    ImGui::SameLine();
    if (ImGui::Button("Restore Defaults")) map.load_defaults();

    ImGui::End();
}

void Editor::draw_spawner(std::vector<std::shared_ptr<Actor>>& actors) {
    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Spawner Panel");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    auto do_spawn = [&](const std::string& type_name) {
        std::string act_name = type_name + "_" + std::to_string(spawn_count++);
        auto new_actor = std::make_unique<EditorPrimitiveActor>(act_name, type_name);
        
        // Setup initial position in front of viewport camera
        new_actor->get_actor_transform().position = { 0.0f, 0.0f, -4.5f };
        new_actor->begin_play();
        
        clear_selection();
        Actor* spawned_raw = new_actor.get();
        select_actor(spawned_raw);
        scene_dirty = true;
        actors.push_back(std::move(new_actor));
        record_scene_addition({ spawned_raw });
    };

    if (ImGui::Button("Spawn Cube", ImVec2(-FLT_MIN, 25.0f))) {
        do_spawn("Cube");
    }
    if (ImGui::Button("Spawn Square", ImVec2(-FLT_MIN, 25.0f))) {
        do_spawn("Square");
    }
    if (ImGui::Button("Spawn Oval", ImVec2(-FLT_MIN, 25.0f))) {
        do_spawn("Oval");
    }
    if (ImGui::Button("Spawn Sprite", ImVec2(-FLT_MIN, 25.0f))) {
        std::string act_name = "Sprite_" + std::to_string(spawn_count++);
        auto new_actor = std::make_unique<SpriteActor>(act_name);
        new_actor->get_actor_transform().position = { 0.0f, 0.0f, -4.5f };
        new_actor->begin_play();
        clear_selection();
        Actor* spawned_raw = new_actor.get();
        select_actor(spawned_raw);
        scene_dirty = true;
        actors.push_back(std::move(new_actor));
        record_scene_addition({ spawned_raw });
    }
    
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Lights");
    ImGui::Separator();
    
    auto do_spawn_light = [&](const std::string& type_name, int type) {
        std::string act_name = type_name + "_" + std::to_string(spawn_count++);
        auto new_actor = std::make_unique<EditorPrimitiveActor>(act_name, "Sphere");
        
        if (type == 1) new_actor->create_component<PointLightComponent>("Light");
        else if (type == 2) new_actor->create_component<SpotLightComponent>("Light");
        else if (type == 3) new_actor->create_component<AreaLightComponent>("Light");
        else if (type == 4) new_actor->create_component<SkyLightComponent>("Light");
        
        new_actor->get_actor_transform().position = { 0.0f, 0.0f, -4.5f };
        new_actor->get_actor_transform().scale = { 0.2f, 0.2f, 0.2f };
        new_actor->begin_play();
        
        clear_selection();
        Actor* spawned_raw = new_actor.get();
        select_actor(spawned_raw);
        scene_dirty = true;
        actors.push_back(std::move(new_actor));
        record_scene_addition({ spawned_raw });
    };

    if (ImGui::Button("Spawn Point Light", ImVec2(-FLT_MIN, 25.0f))) do_spawn_light("PointLight", 1);
    if (ImGui::Button("Spawn Spot Light", ImVec2(-FLT_MIN, 25.0f))) do_spawn_light("SpotLight", 2);
    if (ImGui::Button("Spawn Area Light", ImVec2(-FLT_MIN, 25.0f))) do_spawn_light("AreaLight", 3);
    if (ImGui::Button("Spawn Sky Light", ImVec2(-FLT_MIN, 25.0f))) do_spawn_light("SkyLight", 4);

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Volumetrics");
    ImGui::Separator();
    if (ImGui::Button("Spawn Cone Light (SLR)", ImVec2(-FLT_MIN, 25.0f))) {
        std::string act_name = "ConeLight_" + std::to_string(spawn_count++);
        auto new_actor = std::make_unique<StaticSLRActor>(act_name);
        new_actor->shape = 1;
        new_actor->get_actor_transform().position = { 0.0f, 1.6f, -4.5f };
        new_actor->get_actor_transform().scale = { 2.4f, 3.2f, 2.4f };
        new_actor->begin_play();
        clear_selection();
        Actor* spawned_raw = new_actor.get();
        select_actor(spawned_raw);
        scene_dirty = true;
        actors.push_back(std::move(new_actor));
        record_scene_addition({ spawned_raw });
    }
    if (ImGui::Button("Spawn Static Light Ray (SLR)", ImVec2(-FLT_MIN, 25.0f))) {
        std::string act_name = "LightRay_" + std::to_string(spawn_count++);
        auto new_actor = std::make_unique<StaticSLRActor>(act_name);
        // Drop it in front of the camera, standing upright like a shaft from above.
        new_actor->get_actor_transform().position = { 0.0f, 1.0f, -4.5f };
        new_actor->begin_play();
        clear_selection();
        Actor* spawned_raw = new_actor.get();
        select_actor(spawned_raw);
        scene_dirty = true;
        actors.push_back(std::move(new_actor));
        record_scene_addition({ spawned_raw });
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Audio");
    ImGui::Separator();
    
    if (ImGui::Button("Spawn Audio Source", ImVec2(-FLT_MIN, 25.0f))) {
        std::string act_name = "AudioSource_" + std::to_string(spawn_count++);
        auto new_actor = std::make_unique<EditorPrimitiveActor>(act_name, "Sphere"); // Using Sphere visually
        new_actor->create_component<AudioComponent>("Audio");
        new_actor->get_actor_transform().position = { 0.0f, 0.0f, -4.5f };
        new_actor->get_actor_transform().scale = { 0.2f, 0.2f, 0.2f };
        new_actor->begin_play();
        
        clear_selection();
        Actor* spawned_raw = new_actor.get();
        select_actor(spawned_raw);
        scene_dirty = true;
        actors.push_back(std::move(new_actor));
        record_scene_addition({ spawned_raw });
    }

    if (ImGui::Button("Spawn Sphere", ImVec2(-FLT_MIN, 25.0f))) {
        do_spawn("Sphere");
    }
}

// =============================================================================
//  Preferences
//
//  Currently just the code editor, kept in its own file beside the executable
//  rather than in the scene: which editor someone uses is a property of their
//  machine, not of the project, and committing it would have every collaborator
//  fighting over it.
// =============================================================================

static std::filesystem::path preferences_path() {
    return Platform::executable_dir() / "editor_prefs.json";
}

void Editor::load_preferences() {
    std::ifstream file(preferences_path());
    if (!file.is_open()) return;   // no file yet is the normal first-run case

    try {
        nlohmann::json prefs;
        file >> prefs;
        external_editor_index = prefs.value("external_editor_index", 0);
        const std::string command = prefs.value("external_editor_command", std::string());
        std::snprintf(external_editor_command, sizeof(external_editor_command), "%s", command.c_str());
    } catch (const std::exception& e) {
        // A corrupt preferences file must not stop the editor starting. Defaults
        // are always usable, and the file is rewritten on the next change.
        std::cerr << "[Editor] Ignoring unreadable preferences: " << e.what() << std::endl;
    }
}

void Editor::save_preferences() const {
    nlohmann::json prefs;
    prefs["external_editor_index"] = external_editor_index;
    prefs["external_editor_command"] = std::string(external_editor_command);

    std::ofstream file(preferences_path());
    if (!file.is_open()) return;
    file << prefs.dump(2) << std::endl;
}

void Editor::open_script_file(const std::string& path, int line) {
    // The built-in editor is not a launch at all - it reads the file into the
    // in-engine buffer, which is the behaviour this always had.
    if (external_editor_index == ExternalEditor::kBuiltIn) {
        editing_file_path = path;
        std::ifstream file(path);
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            editing_file_content = buffer.str();
            show_script_editor = true;
        }
        return;
    }

    if (ExternalEditor::open_file(external_editor_index, external_editor_command, path, line)) {
        return;
    }

    // The configured editor could not be launched - uninstalled since it was
    // chosen, or a custom command that does not resolve. Falling back to the
    // built-in editor means the file still opens, which is what the user asked
    // for; saying so means they can go fix the setting.
    std::cerr << "[Editor] Could not launch the configured external editor; "
                 "opening in the built-in editor instead." << std::endl;
    editing_file_path = path;
    std::ifstream file(path);
    if (file.is_open()) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        editing_file_content = buffer.str();
        show_script_editor = true;
    }
}

void Editor::draw_preferences() {
    if (!show_preferences) return;

    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Preferences", &show_preferences)) {
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "Script Editor");
    ImGui::Separator();
    ImGui::TextWrapped("Which editor opens a script when you double-click it in the "
                       "Content Browser.");
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    const std::vector<ExternalEditor::Definition>& editors = ExternalEditor::registry();

    // Which of them are actually installed. Probed once and cached: this walks PATH
    // for every candidate of every editor, which is far too much to redo per frame.
    static std::vector<std::string> resolved;
    static bool probed = false;
    if (!probed) {
        probed = true;
        resolved.reserve(editors.size());
        for (const ExternalEditor::Definition& definition : editors) {
            resolved.push_back(ExternalEditor::resolve_executable(definition));
        }
    }

    const int custom_index = static_cast<int>(editors.size());
    const char* preview = (external_editor_index >= 0 && external_editor_index < custom_index)
        ? editors[static_cast<size_t>(external_editor_index)].display_name
        : "Custom command";

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##external_editor", preview)) {
        for (int i = 0; i < custom_index; ++i) {
            const ExternalEditor::Definition& definition = editors[static_cast<size_t>(i)];
            // The built-in editor and the system handler are always available; the
            // rest are only selectable if they were actually found.
            const bool always_available = (i == ExternalEditor::kBuiltIn ||
                                           i == ExternalEditor::kSystemDefault);
            const bool installed = always_available || !resolved[static_cast<size_t>(i)].empty();

            // Editors that are not installed are listed but disabled, rather than
            // hidden. Seeing "Visual Studio Code (not found)" tells you the engine
            // supports it and you have not installed it; an absent row tells you
            // nothing at all.
            if (!installed) ImGui::BeginDisabled();
            const bool selected = (external_editor_index == i);
            std::string label = definition.display_name;
            if (!always_available) {
                label += installed ? ("  [" + resolved[static_cast<size_t>(i)] + "]")
                                   : "  (not found)";
            }
            if (ImGui::Selectable(label.c_str(), selected) && installed) {
                external_editor_index = i;
                save_preferences();
            }
            if (selected) ImGui::SetItemDefaultFocus();
            if (!installed) ImGui::EndDisabled();
        }

        const bool custom_selected = (external_editor_index >= custom_index);
        if (ImGui::Selectable("Custom command...", custom_selected)) {
            external_editor_index = custom_index;
            save_preferences();
        }
        ImGui::EndCombo();
    }

    if (external_editor_index >= custom_index) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextWrapped("Command to run. {file} is the script's absolute path and "
                           "{line} the line number. An argument containing {line} is "
                           "dropped when no line is known, and {file} is appended if "
                           "you do not mention it.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##custom_editor_command", external_editor_command,
                             sizeof(external_editor_command))) {
            save_preferences();
        }
        ImGui::TextDisabled("Examples:");
        ImGui::TextDisabled("  code --goto {file}:{line}");
        ImGui::TextDisabled("  emacsclient +{line} {file}");
        ImGui::TextDisabled("  /opt/myeditor/bin/edit {file}");
        // No shell is involved, which is worth saying: people reach for pipes and
        // && here, and they will not work.
        ImGui::TextDisabled("Run directly, not through a shell - no pipes or &&.");
    }

    if (external_editor_index != ExternalEditor::kBuiltIn) {
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::Separator();
        ImGui::TextWrapped("Scripts open outside the engine. Edits are picked up by "
                           "the hot-reload watcher when you save.");

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        // Proving it works before the user needs it to is worth a button: a wrong
        // custom command otherwise only reveals itself the first time they
        // double-click a script and nothing happens.
        if (ImGui::Button("Test (open this preferences file)", ImVec2(-FLT_MIN, 0.0f))) {
            const std::string test_target = preferences_path().string();
            if (!ExternalEditor::open_file(external_editor_index, external_editor_command,
                                           test_target, 1)) {
                preferences_test_message = "Could not launch that editor.";
            } else {
                preferences_test_message = "Launched. If nothing opened, check the command.";
            }
        }
        if (!preferences_test_message.empty()) {
            ImGui::TextWrapped("%s", preferences_test_message.c_str());
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::Separator();
    if (ImGui::Button("Re-scan for editors", ImVec2(-FLT_MIN, 0.0f))) {
        // For the case where someone installs their editor while the engine is up.
        probed = false;
    }

    ImGui::End();
}
