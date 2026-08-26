#pragma once

#include "world/component.hpp"
#include "core/math.hpp"
#include <memory>
#include <string>
#include <vector>

// A rectangle in canvas-logical pixels, with the origin at the top-left of the
// viewport and y growing downward - the convention every 2D UI uses, and the one
// mouse coordinates already arrive in.
struct UIRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool contains(float px, float py) const {
        return px >= x && px <= x + width && py >= y && py <= y + height;
    }
};

// What the canvas is told about the pointer for the frame. Passed in rather than
// read from the window so the same code drives the fullscreen standalone game and
// the editor's viewport panel, which sits at an arbitrary offset inside the window.
struct UIInputState {
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    bool  mouse_down = false;      // held this frame
    bool  mouse_pressed = false;   // went down this frame
    bool  mouse_released = false;  // came up this frame
    // False in the editor while not playing, so an authored layout can be previewed
    // without its buttons reacting to the mouse used to edit them.
    bool  interactive = true;
};

// One element of a canvas.
//
// The rectangle is described the way every modern UI describes one: two anchors
// given as fractions of the parent's rect, plus a pixel offset from each. That
// single mechanism covers pinning to a corner, centring, and stretching to fill -
// so a health bar pinned to the bottom-left and a full-screen dimmer are the same
// kind of object with different numbers.
struct UIWidget {
    // Numbering is part of the scene file format, so new kinds append.
    enum Type : int {
        // A rectangle. The container everything else is parented to, and the
        // background of a dialog or a HUD panel.
        Widget_Panel       = 0,
        Widget_Label       = 1,
        Widget_Image       = 2,
        Widget_Button      = 3,
        Widget_Checkbox    = 4,
        // Draggable value between min_value and max_value.
        Widget_Slider      = 5,
        // Read-only bar. Health, loading, cooldown.
        Widget_ProgressBar = 6,
        // Single-line editable text.
        Widget_TextField   = 7,
        Widget_Count       = 8
    };

    enum Align : int { Align_Start = 0, Align_Center = 1, Align_End = 2 };

    static const char* type_name(int type);
    static bool type_has_text(int type);
    static bool type_has_value(int type);
    static bool type_is_interactive(int type);

    int type = Widget_Panel;
    // Addressed by name from scripts, so it has to be unique within a canvas for
    // lookups to be meaningful. The editor enforces that when creating widgets.
    std::string name = "Widget";
    bool visible = true;
    // A non-interactive button still draws but ignores the mouse, which is how a
    // greyed-out control is expressed.
    bool interactive = true;

    // --- Rect ---------------------------------------------------------------
    // Fractions of the parent rect. Equal min and max on an axis pins that axis and
    // the offsets become a position and a size; different values stretch, and the
    // offsets become insets from each edge.
    Vector2 anchor_min = { 0.0f, 0.0f };
    Vector2 anchor_max = { 0.0f, 0.0f };
    Vector2 offset_min = { 0.0f, 0.0f };
    Vector2 offset_max = { 160.0f, 40.0f };

    // --- Appearance ---------------------------------------------------------
    Vector4 background_color = { 0.08f, 0.09f, 0.11f, 0.85f };
    Vector4 border_color     = { 1.0f, 1.0f, 1.0f, 0.22f };
    float border_thickness = 1.0f;
    float corner_radius = 4.0f;

    // --- Text ---------------------------------------------------------------
    std::string text;
    Vector4 text_color = { 0.92f, 0.94f, 0.97f, 1.0f };
    // Multiplier on the UI font's base size, so a title and a caption differ by a
    // number rather than by needing separate fonts.
    float font_scale = 1.0f;
    int h_align = Align_Center;
    int v_align = Align_Center;
    // Wraps at the widget's width instead of overflowing. Off by default: a HUD
    // label that suddenly wraps is worse than one that runs long.
    bool word_wrap = false;
    // Inset applied to the text rect on both axes, so text does not touch a border.
    float padding = 6.0f;

    // --- Image --------------------------------------------------------------
    // Loaded through the resource manager the first time the widget is drawn. Empty
    // draws the background colour alone, which is what makes a Panel and an Image
    // with no texture the same thing.
    std::string image_path;
    // Multiplied into the texture, so one white sprite can be tinted per instance.
    Vector4 image_tint = { 1.0f, 1.0f, 1.0f, 1.0f };

    // --- Interactive feedback ------------------------------------------------
    Vector4 hover_color   = { 0.18f, 0.34f, 0.52f, 0.95f };
    Vector4 pressed_color = { 0.10f, 0.22f, 0.36f, 1.0f };
    Vector4 disabled_color = { 0.12f, 0.12f, 0.13f, 0.6f };

    // --- Value --------------------------------------------------------------
    // Slider position, progress fraction, or checkbox state (0 or 1). Always stored
    // in real units; sliders and bars convert to a fraction with the range below.
    float value = 0.0f;
    float min_value = 0.0f;
    float max_value = 1.0f;
    Vector4 fill_color = { 0.30f, 0.62f, 0.90f, 1.0f };

    std::vector<std::unique_ptr<UIWidget>> children;

    // --- Transient runtime state --------------------------------------------
    // Rebuilt every frame; not serialised. Public so the editor can show what the
    // layout resolved to, which is the only way to debug an anchor that fights its
    // parent.
    UIRect computed_rect;
    bool hovered = false;
    bool pressed = false;
    // True for exactly the frame a click completed on this widget.
    bool clicked = false;
    // True for the frame value changed through user interaction.
    bool value_changed = false;
    bool focused = false;

    // Normalised position of `value` within [min_value, max_value].
    float fraction() const;
    void set_fraction(float t);
};

// A screen-space UI attached to an actor.
//
// The canvas is authored at reference_resolution and scaled to whatever the
// viewport turns out to be, so a HUD laid out for 1080p is not a row of unreadable
// specks at 4K. Scripts address widgets by name; nothing outside this component
// needs to know the tree exists.
class UICanvasComponent : public ActorComponent {
public:
    enum ScaleMode : int {
        // One canvas unit is one screen pixel. Correct for a debug overlay, wrong
        // for anything the player is meant to read at any resolution.
        Scale_ConstantPixel = 0,
        // Canvas units are scaled so the reference resolution fills the viewport.
        Scale_WithScreenSize = 1
    };

    UICanvasComponent(Actor* owner, const std::string& name);
    virtual ~UICanvasComponent();

    // Deliberately empty: the canvas is laid out and submitted from the render
    // thread's frame, not from the parallel actor tick, because drawing and reading
    // the mouse both have to happen on the thread that owns the UI frame.
    virtual void tick(float delta_time) override {}

    Vector2 reference_resolution = { 1920.0f, 1080.0f };
    int scale_mode = Scale_WithScreenSize;
    // 0 matches the viewport's width, 1 its height, and values between interpolate.
    // Height-matching keeps a HUD the same relative size on an ultrawide monitor.
    float match_width_or_height = 0.5f;
    // Canvases are drawn in ascending order, so a pause menu with a higher order
    // covers the HUD rather than fighting it.
    int sort_order = 0;
    bool visible = true;
    // Draws the canvas in the editor viewport while not playing, for authoring. It
    // never takes input in that mode - the mouse belongs to the editor.
    bool show_in_editor = true;

    std::vector<std::unique_ptr<UIWidget>> roots;

    // Lays the tree out inside the given screen rectangle and submits it to the UI
    // frame. Must be called on the thread that owns the UI frame, once per frame.
    void render(const UIRect& screen_rect, const UIInputState& input);

    // Widgets that completed a click during the last render(), and those whose value
    // the user changed. Drained by the engine, which turns them into script calls.
    const std::vector<std::string>& get_clicked_widgets() const { return clicked_widgets; }
    const std::vector<std::string>& get_changed_widgets() const { return changed_widgets; }
    void clear_events();

    // True when the pointer was over an interactive widget during the last render.
    // The engine uses this to stop a click that hit a button from also falling
    // through to the viewport and selecting whatever is behind it.
    bool consumed_mouse() const { return mouse_consumed; }

    // True while any canvas anywhere holds keyboard focus, which happens only when
    // a text field is being typed into. Gameplay reads the keyboard directly rather
    // than through the UI, so without this the player walks off while typing.
    static bool any_keyboard_focus();

    // --- Widget access -------------------------------------------------------
    // Depth-first search by name. Null when nothing matches; every script-facing
    // setter tolerates that rather than treating a renamed widget as fatal.
    UIWidget* find(const std::string& widget_name);
    const UIWidget* find(const std::string& widget_name) const;
    // Creates a widget under `parent`, or at the root when parent is null. The name
    // is made unique against the rest of the canvas.
    UIWidget* add_widget(int type, UIWidget* parent, const std::string& desired_name);
    // Removes a widget and everything under it. Returns false if it is not in this
    // canvas.
    bool remove_widget(UIWidget* widget);
    // The widget holding `child`, or null when it is a root.
    UIWidget* parent_of(UIWidget* child);
    // Whether this pointer still names a widget in this canvas. Anything holding a
    // widget across frames - the editor's selection - has to ask before it
    // dereferences, because the tree can be edited between them.
    bool contains(const UIWidget* widget) const;
    bool name_in_use(const std::string& candidate, const UIWidget* ignore = nullptr) const;
    std::string make_unique_name(const std::string& desired) const;

private:
    // Resolves this widget's rect against its parent's and recurses. Also clears the
    // per-frame interaction flags, so hit testing starts from a clean slate.
    void layout(UIWidget& widget, const UIRect& parent_rect);
    // Topmost interactive widget under the pointer, searched in reverse draw order.
    UIWidget* hit_test(UIWidget& widget, float mouse_x, float mouse_y);
    // Applies hover, press, click, drag and text entry for the frame.
    void update_interaction(const UIInputState& input);
    // draw_list is an ImDrawList*, kept opaque so ImGui stays out of this header.
    void draw_widget(const UIWidget& widget, void* draw_list, float scale, const UIRect& screen_rect);

    std::vector<std::string> clicked_widgets;
    std::vector<std::string> changed_widgets;
    bool mouse_consumed = false;
    // The widget the pointer went down on, so a click only counts if it is released
    // over the same one - dragging off a button has to cancel it.
    UIWidget* press_target = nullptr;
    UIWidget* focus_target = nullptr;
};
