#pragma once

#include <set>
#include <functional>

#include "world/actor.hpp"
#include "world/material.hpp"
#include "core/command.hpp"
#include "core/visual_script_editor.hpp"
#include "navigation/navmesh.hpp"
#include "renderer/lightmapper.hpp"
#include <vector>
#include <memory>
#include <string>

class Renderer;
// Opaque declaration of the engine's run state. engine.hpp includes this header,
// so it cannot be included back; a scoped enum has a fixed underlying type, which
// makes forward-declaring it legal. Passing the enum rather than an int is what
// stops the toolbar from decoding states positionally - adding a state to the enum
// previously shifted every value and made the Editor state read as PlayInEditor.
enum class EngineState;

enum class EditorRequest {
    None,
    Play,
    Pause,
    Resume,
    Stop
};

class Editor {
public:
    Editor();
    ~Editor();

    // Renders the full editor panels
    EditorRequest render(std::vector<std::shared_ptr<Actor>>& actors, unsigned int viewport_texture_id, unsigned int logo_texture_id, bool& out_screenshot_requested, bool& out_viewport_clicked, float& out_ndc_x, float& out_ndc_y, const Matrix4x4& view, const Matrix4x4& proj, Renderer* renderer, EngineState engine_state);

    const std::vector<Actor*>& get_selected_actors() const { return selected_actors; }
    void clear_selection() { selected_actors.clear(); }
    void select_actor(Actor* actor) { selected_actors.push_back(actor); }
    // Aspect ratio (width / height) of the logo texture, set once by the engine
    // after it loads the image. The panel used to draw the logo into a fixed
    // 200x50 box regardless of its real proportions, which squashed the square
    // brand mark into a flat ellipse.
    float logo_aspect = 1.0f;
    // Display name of the environment map currently chosen in the sky library.
    std::string sky_label_current = "";
    // Set each frame from render(); panels drawn from helper methods need it.
    Renderer* active_renderer = nullptr;
    bool is_actor_selected(Actor* actor) const;

    bool is_wireframe_mode() const { return wireframe_mode; }
    void set_wireframe_mode(bool mode) { wireframe_mode = mode; }
    
    bool is_tesla_mode() const { return tesla_mode; }
    void set_tesla_mode(bool mode) { tesla_mode = mode; }

    void undo();
    void redo();
    bool can_undo() const { return !undo_stack.empty(); }
    bool can_redo() const { return !redo_stack.empty(); }

    // The live scene list, refreshed every frame from render(). Undo/redo needs it to
    // put actors back and to drop selection entries that no longer exist.
    std::vector<std::shared_ptr<Actor>>* active_actors = nullptr;

    // Records actors that just entered the scene (spawn, duplicate, paste) or are
    // about to leave it (delete), so the operation can be undone. Call the removal
    // one *before* erasing them from the scene list.
    void record_scene_addition(const std::vector<Actor*>& added);
    void record_scene_removal(const std::vector<Actor*>& removed);

    // Begins/ends a Details-panel edit. begin captures the selection's properties,
    // end diffs against them and pushes a single undo entry if anything changed.
    void begin_property_edit();
    void end_property_edit();

    // Fade duration the Animation panel's "Fade To" buttons use, in seconds. Lives
    // on the editor rather than on the player because it is an authoring
    // preference, not part of any actor's state.
    float animation_fade_seconds = 0.25f;

    // Widget selected in the UI Canvas panel. A raw pointer into the canvas tree,
    // so it is re-validated against the canvas every frame before being used - the
    // tree can be edited, and a deleted widget must not be drawn from.
    struct UIWidget* selected_widget = nullptr;

    // --- Terrain sculpting --------------------------------------------------
    // Which brush the viewport is currently applying, or -1 for none. While a brush
    // is active the gizmo is suppressed, because dragging in the viewport means
    // painting rather than moving.
    enum TerrainBrush : int {
        TerrainBrush_None    = -1,
        TerrainBrush_Raise   = 0,
        TerrainBrush_Lower   = 1,
        TerrainBrush_Smooth  = 2,
        TerrainBrush_Flatten = 3,
        TerrainBrush_Paint   = 4,
        TerrainBrush_Foliage = 5
    };
    int terrain_brush = TerrainBrush_None;
    int terrain_paint_layer = 0;
    float terrain_brush_radius = 8.0f;
    float terrain_brush_strength = 4.0f;
    float terrain_flatten_height = 0.0f;
    bool terrain_foliage_erase = false;
    // Applies the active brush where the cursor is pointing. Called from the
    // viewport, which is the only place that knows the ray.
    void apply_terrain_brush(std::vector<std::shared_ptr<Actor>>& actors,
                             const Matrix4x4& view, float ndc_x, float ndc_y, float aspect);
    void draw_terrain_panel(class TerrainComponent* terrain);

    // LOD generation settings for the LOD Group panel, plus the last run's result.
    int lod_generate_levels = 3;
    float lod_generate_ratio = 0.4f;
    std::string lod_generate_report;

    int gizmo_mode = 0; // 0 = Translate, 1 = Rotate, 2 = Scale
    bool gizmo_local_space = false;
    // Outliner name filter. Scenes grow past what fits on screen quickly, and the
    // list was previously unsearchable.
    char outliner_filter[128] = "";
    
    // Screen-space rectangle of the 3D viewport panel, recorded every frame. The
    // game UI is laid out inside it so a HUD lines up with the view the player
    // would see, rather than with the whole editor window.
    float viewport_screen_x = 0.0f;
    float viewport_screen_y = 0.0f;
    float viewport_screen_w = 0.0f;
    float viewport_screen_h = 0.0f;

    std::string current_scene_path = "";

    // Unsaved-change tracking. Set by every scene mutation, cleared on save/load, so
    // closing the editor can offer to save instead of silently discarding work.
    bool scene_dirty = false;
    void mark_scene_dirty() { scene_dirty = true; }
    bool has_unsaved_changes() const { return scene_dirty; }
    // Saves to the current path, prompting for one if the scene has never been saved.
    // Returns false if the user cancelled the file dialog.
    bool save_scene(std::vector<std::shared_ptr<Actor>>& actors);
    // --- Prefabs ------------------------------------------------------------
    // Saves one actor as a reusable .prefab asset under Content/, and instantiates
    // one back into the scene. Instantiation is undoable like any other spawn.
    bool create_prefab_from_actor(Actor* actor);
    Actor* instantiate_prefab(std::vector<std::shared_ptr<Actor>>& actors, const std::string& filepath);
    static bool is_prefab_file(const std::string& path);

    // Drag-and-drop model import.
    static bool is_model_file(const std::string& path);
    Actor* spawn_model_actor(std::vector<std::shared_ptr<Actor>>& actors, const std::string& filepath);

    // --- Build / packaging --------------------------------------------------
    // Settings gathered by the Build dialog before packaging a standalone game.
    struct BuildSettings {
        int  target_platform = 0;      // 0 = Linux (ELF), 1 = Windows (EXE)
        int  resolution_index = 1;     // index into the resolution table
        bool fullscreen = false;
        bool vsync = true;
        int  quality_preset = 2;       // 0 Low, 1 Medium, 2 High, 3 Ultra
        bool enable_ssr = true;
        bool enable_bloom = true;
        bool enable_taa = true;
        bool enable_gi = true;
        bool include_editor_content = true;   // ship the sky library
        bool strip_debug = true;
        char product_name[64] = "MyGame";
    };
    BuildSettings build_settings;
    bool show_build_dialog = false;
    // --- Navigation ---------------------------------------------------------
    // Navigation window, opened from the Settings menu. The build parameters live
    // here rather than on the navmesh because they are authoring state: the navmesh
    // keeps whatever it was last built with.
    bool show_navigation_window = false;
    NavBuildSettings nav_settings;
    // Draws the walkable surface over the viewport, which is the only way to see
    // whether a build did what was intended.
    bool show_navmesh_overlay = false;
    std::string nav_build_report;
    void draw_navigation_window(std::vector<std::shared_ptr<Actor>>& actors);
    // Projects the navmesh's walkable cells onto the viewport. Called with the same
    // matrices the viewport was drawn with.
    void draw_navmesh_overlay(const Matrix4x4& view, const Matrix4x4& proj);

    // Collision Layers window, opened from the Settings menu. Layer names and the
    // interaction matrix are project-wide, so they live in the engine options file
    // rather than in any one scene.
    bool show_collision_layers = false;
    void draw_collision_layers();

    // Audio Buses window, opened from the Settings menu.
    bool show_audio_buses = false;
    void draw_audio_buses();

    // Baked lighting window, opened from the Rendering menu.
    bool show_lighting_bake = false;
    Lightmapper::BakeSettings bake_settings;
    std::string bake_report;
    void draw_lighting_bake(std::vector<std::shared_ptr<Actor>>& actors);

    // Input Bindings window, opened from the Settings menu.
    bool show_input_bindings = false;
    // Index of the binding currently listening for a key press, encoded as
    // action * 1000 + binding. -1 when nothing is being rebound.
    int input_capture_target = -1;
    void draw_build_dialog(std::vector<std::shared_ptr<Actor>>& actors);
    bool run_build(std::vector<std::shared_ptr<Actor>>& actors, const std::string& out_dir, std::string& out_report);

private:
    std::vector<Actor*> selected_actors;
    bool wireframe_mode = false;
    bool tesla_mode = false;
    int spawn_count = 0;

    std::vector<std::unique_ptr<Command>> undo_stack;
    std::vector<std::unique_ptr<Command>> redo_stack;
    std::vector<TransformCommand::ActorTransformState> drag_start_states;
    bool is_dragging = false;

    // Details-panel edit in progress, captured at the moment a widget went active.
    std::vector<ActorPropertyState> property_edit_start;
    bool property_edit_active = false;

    // Pushes a command and invalidates the redo branch, which is what every new
    // action must do - keeping a redo stack that no longer applies would replay
    // changes against a scene that has moved on.
    void push_command(std::unique_ptr<Command> cmd);
    // Drops selection entries whose actor is no longer in the scene. Selection holds
    // raw pointers, so an undone spawn would otherwise leave a dangling one.
    void prune_selection();

    void draw_menu_bar(std::vector<std::shared_ptr<Actor>>& actors, bool& out_screenshot_requested);
    
    void draw_outliner(std::vector<std::shared_ptr<Actor>>& actors);
    void draw_properties();
    // Authoring UI for one game UI canvas: the widget tree, plus the properties of
    // whichever widget is selected in it.
    void draw_ui_canvas_panel(class UICanvasComponent* canvas);
    void draw_ui_widget_tree(class UICanvasComponent* canvas, struct UIWidget* widget);
    void draw_ui_widget_properties(class UICanvasComponent* canvas, struct UIWidget* widget);
    void draw_viewport(std::vector<std::shared_ptr<Actor>>& actors, unsigned int texture_id, bool& out_clicked, float& out_ndc_x, float& out_ndc_y, const Matrix4x4& view, const Matrix4x4& proj, Renderer* renderer);
    void draw_content_browser(Renderer* renderer);
    void draw_spawner(std::vector<std::shared_ptr<Actor>>& actors);
    void draw_profiler(Renderer* renderer, const std::vector<std::shared_ptr<Actor>>& actors);
    void draw_input_bindings();
    void draw_script_editor();
    void draw_material_editor();
    EditorRequest draw_toolbar(EngineState engine_state);

    std::string editing_file_path = "";
    std::string editing_file_content = "";
    bool show_script_editor = false;
    bool show_preferences = false;

    // --- Outliner folders ---------------------------------------------------
    // Every folder that exists, as a '/'-separated path. Held here rather than
    // derived from the actors because an empty folder has nothing to derive it
    // from, and a folder you just made is empty by definition.
    std::set<std::string> outliner_folders;
    // Folder a newly created one is nested under, and the buffer its name is typed
    // into. Cleared when the popup closes.
    std::string pending_folder_parent;
    char new_folder_name[128] = "";
    bool open_new_folder_popup = false;
    // Folder being renamed, and the buffer for it.
    std::string renaming_folder;
    // Set by a context menu and applied after the tree walk finishes.
    std::string pending_folder_delete;

    // --- Duplicate / copy / paste -------------------------------------------
    // Actors copied with Ctrl+C, held as full records rather than as pointers so
    // pasting still works after the originals have been deleted.
    std::vector<std::shared_ptr<Actor>> actor_clipboard;
    // Renaming in the Outliner: which actor, and the buffer being typed into.
    Actor* renaming_actor = nullptr;
    char renaming_actor_buf[128] = "";
    // Deferred so the actor list is not mutated while the tree walking it is live.
    bool pending_duplicate = false;
    bool pending_paste = false;
    // Set when a rename starts, so the field takes focus on its first frame.
    bool rename_focus_pending = false;

    void duplicate_selected_actors(std::vector<std::shared_ptr<Actor>>& actors);
    void copy_selected_actors();
    void paste_actors(std::vector<std::shared_ptr<Actor>>& actors);

public:
    // Exposed for the self-test: the "Name", "Name (1)", "Name (2)" sequence is the
    // whole of the duplicate/rename naming contract, and it is worth pinning.
    static std::string next_available_name(const std::string& desired,
                                           const std::function<bool(const std::string&)>& taken);
private:
    char renaming_folder_buf[128] = "";

    // Recursively draws one folder and everything filed under it.
    void draw_outliner_folder(const std::string& path, const std::string& display_name,
                              std::vector<std::shared_ptr<Actor>>& actors);
    // Draws one actor row, with its selection, context menu and drag source.
    void draw_outliner_actor(Actor* actor);
    // Accepts an actor dropped onto a folder row (or the root).
    void accept_actor_drop(const std::string& target_folder);
    // Renames a folder and re-files everything under it, including nested folders.
    void rename_outliner_folder(const std::string& old_path, const std::string& new_name,
                                std::vector<std::shared_ptr<Actor>>& actors);
    // Removes a folder; its contents move up to its parent rather than being lost.
    void delete_outliner_folder(const std::string& path, std::vector<std::shared_ptr<Actor>>& actors);
    // True if `actor` passes the current outliner search filter.
    bool actor_matches_filter(const Actor* actor) const;

    // --- External code editor -----------------------------------------------
    // Which editor script files open in. Index into ExternalEditor::registry(), or
    // past its end for the custom command below. Persisted, because nobody wants to
    // pick their editor again every time the engine starts.
    int external_editor_index = 0;              // 0 = the built-in editor
    char external_editor_command[512] = "";     // used when the index is "Custom"
    // Opens `path` in whichever editor is configured, falling back to the built-in
    // one if the external launch fails. `line` is 1-based; 0 means no particular line.
    void open_script_file(const std::string& path, int line = 0);
    void load_preferences();
    void save_preferences() const;
    void draw_preferences();
    std::string preferences_test_message;
    bool show_visual_script_editor = false;
    bool show_material_editor = false;
    std::shared_ptr<Material> active_material = std::make_shared<Material>();

    // Content browser state
    std::string selected_content_file = "";
    std::string content_browser_path = "Content"; // current browsed directory
    bool rename_mode = false;
    char rename_buf[256] = "";
    bool new_folder_mode = false;
    char new_folder_buf[256] = "NewFolder";

    VisualScriptEditor visual_script_editor;
};
