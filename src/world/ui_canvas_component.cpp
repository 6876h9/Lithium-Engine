#include "world/ui_canvas_component.hpp"
#include "core/resource_manager.hpp"
#include "core/texture_resource.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// Logical pixel height of the UI font at font_scale 1. Fixed rather than taken from
// ImGui's current font size so a canvas laid out in the editor looks the same in a
// standalone build, where the editor's font settings do not exist.
constexpr float kBaseFontSize = 18.0f;

ImU32 to_col32(const Vector4& c) {
    auto channel = [](float v) {
        const float clamped = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<int>(clamped * 255.0f + 0.5f);
    };
    return IM_COL32(channel(c.x), channel(c.y), channel(c.z), channel(c.w));
}

float clamp01(float v) { return (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Canvases only take keyboard input while a text field is focused, and only one
// field anywhere can hold focus. Tracked globally so gameplay - which reads the
// keyboard directly rather than through the UI - can tell that the player is
// typing and stop walking them into a wall.
int g_keyboard_focus_count = 0;

} // namespace

const char* UIWidget::type_name(int type) {
    switch (type) {
        case Widget_Panel:       return "Panel";
        case Widget_Label:       return "Label";
        case Widget_Image:       return "Image";
        case Widget_Button:      return "Button";
        case Widget_Checkbox:    return "Checkbox";
        case Widget_Slider:      return "Slider";
        case Widget_ProgressBar: return "Progress Bar";
        case Widget_TextField:   return "Text Field";
        default:                 return "Unknown";
    }
}

bool UIWidget::type_has_text(int type) {
    return type == Widget_Label || type == Widget_Button ||
           type == Widget_Checkbox || type == Widget_TextField ||
           type == Widget_Panel || type == Widget_ProgressBar;
}

bool UIWidget::type_has_value(int type) {
    return type == Widget_Slider || type == Widget_ProgressBar || type == Widget_Checkbox;
}

bool UIWidget::type_is_interactive(int type) {
    return type == Widget_Button || type == Widget_Checkbox ||
           type == Widget_Slider || type == Widget_TextField;
}

float UIWidget::fraction() const {
    const float span = max_value - min_value;
    if (std::abs(span) < 1e-6f) return 0.0f;
    return clamp01((value - min_value) / span);
}

void UIWidget::set_fraction(float t) {
    value = min_value + (max_value - min_value) * clamp01(t);
}

// --- Canvas ----------------------------------------------------------------

UICanvasComponent::UICanvasComponent(Actor* owner, const std::string& name)
    : ActorComponent(owner, name) {}

UICanvasComponent::~UICanvasComponent() {
    // A canvas destroyed while one of its fields held focus would otherwise leave
    // the global count high forever, and gameplay input dead with it.
    if (focus_target) g_keyboard_focus_count = std::max(0, g_keyboard_focus_count - 1);
}

bool UICanvasComponent::any_keyboard_focus() {
    return g_keyboard_focus_count > 0;
}

void UICanvasComponent::clear_events() {
    clicked_widgets.clear();
    changed_widgets.clear();
}

// --- Tree helpers ----------------------------------------------------------

namespace {

UIWidget* find_in(std::vector<std::unique_ptr<UIWidget>>& list, const std::string& name) {
    for (auto& w : list) {
        if (!w) continue;
        if (w->name == name) return w.get();
        if (UIWidget* found = find_in(w->children, name)) return found;
    }
    return nullptr;
}

bool name_used_in(const std::vector<std::unique_ptr<UIWidget>>& list,
                  const std::string& candidate, const UIWidget* ignore) {
    for (const auto& w : list) {
        if (!w) continue;
        if (w.get() != ignore && w->name == candidate) return true;
        if (name_used_in(w->children, candidate, ignore)) return true;
    }
    return false;
}

bool remove_from(std::vector<std::unique_ptr<UIWidget>>& list, UIWidget* target) {
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->get() == target) {
            list.erase(it);
            return true;
        }
        if (remove_from((*it)->children, target)) return true;
    }
    return false;
}

UIWidget* parent_in(std::vector<std::unique_ptr<UIWidget>>& list, UIWidget* child, UIWidget* current) {
    for (auto& w : list) {
        if (!w) continue;
        if (w.get() == child) return current;
        if (UIWidget* found = parent_in(w->children, child, w.get())) return found;
    }
    return nullptr;
}

// Does this widget (or anything under it) still exist in the canvas? Retained
// pointers - the press and focus targets - have to be checked before use, because
// the tree can be edited between frames.
bool contains_widget(const std::vector<std::unique_ptr<UIWidget>>& list, const UIWidget* target) {
    for (const auto& w : list) {
        if (!w) continue;
        if (w.get() == target) return true;
        if (contains_widget(w->children, target)) return true;
    }
    return false;
}

} // namespace

UIWidget* UICanvasComponent::find(const std::string& widget_name) {
    return find_in(roots, widget_name);
}

const UIWidget* UICanvasComponent::find(const std::string& widget_name) const {
    return const_cast<UICanvasComponent*>(this)->find(widget_name);
}

bool UICanvasComponent::contains(const UIWidget* widget) const {
    return widget && contains_widget(roots, widget);
}

bool UICanvasComponent::name_in_use(const std::string& candidate, const UIWidget* ignore) const {
    return name_used_in(roots, candidate, ignore);
}

std::string UICanvasComponent::make_unique_name(const std::string& desired) const {
    if (!name_in_use(desired)) return desired;
    for (int suffix = 1; suffix < 10000; ++suffix) {
        std::string candidate = desired + std::to_string(suffix);
        if (!name_in_use(candidate)) return candidate;
    }
    return desired;
}

UIWidget* UICanvasComponent::parent_of(UIWidget* child) {
    return parent_in(roots, child, nullptr);
}

UIWidget* UICanvasComponent::add_widget(int type, UIWidget* parent, const std::string& desired_name) {
    auto widget = std::make_unique<UIWidget>();
    widget->type = type;
    widget->name = make_unique_name(desired_name.empty() ? UIWidget::type_name(type) : desired_name);

    // Sensible starting geometry per kind, so a freshly added widget is visible and
    // roughly the right shape rather than a zero-sized rectangle at the origin.
    switch (type) {
        case UIWidget::Widget_Panel:
            widget->offset_max = { 320.0f, 200.0f };
            widget->interactive = false;
            break;
        case UIWidget::Widget_Label:
            widget->offset_max = { 240.0f, 32.0f };
            widget->background_color = { 0.0f, 0.0f, 0.0f, 0.0f };
            widget->border_thickness = 0.0f;
            widget->text = "Label";
            widget->h_align = UIWidget::Align_Start;
            widget->interactive = false;
            break;
        case UIWidget::Widget_Image:
            widget->offset_max = { 128.0f, 128.0f };
            widget->background_color = { 0.0f, 0.0f, 0.0f, 0.0f };
            widget->border_thickness = 0.0f;
            widget->interactive = false;
            break;
        case UIWidget::Widget_Button:
            widget->offset_max = { 180.0f, 44.0f };
            widget->text = "Button";
            widget->background_color = { 0.13f, 0.16f, 0.21f, 0.95f };
            break;
        case UIWidget::Widget_Checkbox:
            widget->offset_max = { 200.0f, 30.0f };
            widget->text = "Checkbox";
            widget->h_align = UIWidget::Align_Start;
            widget->background_color = { 0.0f, 0.0f, 0.0f, 0.0f };
            widget->border_thickness = 0.0f;
            widget->max_value = 1.0f;
            break;
        case UIWidget::Widget_Slider:
            widget->offset_max = { 240.0f, 28.0f };
            widget->background_color = { 0.06f, 0.07f, 0.09f, 0.9f };
            widget->max_value = 1.0f;
            widget->value = 0.5f;
            break;
        case UIWidget::Widget_ProgressBar:
            widget->offset_max = { 240.0f, 22.0f };
            widget->background_color = { 0.06f, 0.07f, 0.09f, 0.9f };
            widget->max_value = 1.0f;
            widget->value = 1.0f;
            widget->interactive = false;
            break;
        case UIWidget::Widget_TextField:
            widget->offset_max = { 240.0f, 34.0f };
            widget->background_color = { 0.05f, 0.06f, 0.08f, 0.95f };
            widget->h_align = UIWidget::Align_Start;
            break;
        default:
            break;
    }

    UIWidget* raw = widget.get();
    if (parent) parent->children.push_back(std::move(widget));
    else roots.push_back(std::move(widget));
    return raw;
}

bool UICanvasComponent::remove_widget(UIWidget* widget) {
    if (!widget) return false;
    if (press_target == widget) press_target = nullptr;
    if (focus_target == widget) {
        g_keyboard_focus_count = std::max(0, g_keyboard_focus_count - 1);
        focus_target = nullptr;
    }
    return remove_from(roots, widget);
}

// --- Layout ----------------------------------------------------------------

void UICanvasComponent::layout(UIWidget& widget, const UIRect& parent_rect) {
    // Two anchors as fractions of the parent plus a pixel offset from each. Equal
    // anchors on an axis pin that axis and the offsets read as position and size;
    // different anchors stretch, and the offsets read as insets.
    const float x0 = parent_rect.x + parent_rect.width  * widget.anchor_min.x + widget.offset_min.x;
    const float y0 = parent_rect.y + parent_rect.height * widget.anchor_min.y + widget.offset_min.y;
    const float x1 = parent_rect.x + parent_rect.width  * widget.anchor_max.x + widget.offset_max.x;
    const float y1 = parent_rect.y + parent_rect.height * widget.anchor_max.y + widget.offset_max.y;

    widget.computed_rect.x = std::min(x0, x1);
    widget.computed_rect.y = std::min(y0, y1);
    widget.computed_rect.width  = std::abs(x1 - x0);
    widget.computed_rect.height = std::abs(y1 - y0);

    widget.clicked = false;
    widget.value_changed = false;
    widget.hovered = false;

    for (auto& child : widget.children) {
        if (child) layout(*child, widget.computed_rect);
    }
}

UIWidget* UICanvasComponent::hit_test(UIWidget& widget, float mx, float my) {
    if (!widget.visible) return nullptr;

    // Children draw over their parent and later siblings over earlier ones, so the
    // pointer has to be offered to them in exactly the reverse of draw order or a
    // button would be blocked by the panel it sits on.
    for (auto it = widget.children.rbegin(); it != widget.children.rend(); ++it) {
        if (!*it) continue;
        if (UIWidget* hit = hit_test(**it, mx, my)) return hit;
    }

    if (widget.interactive && widget.computed_rect.contains(mx, my)) return &widget;
    return nullptr;
}

// --- Interaction -----------------------------------------------------------

void UICanvasComponent::update_interaction(const UIInputState& input) {
    // A retained pointer can outlive the widget it names if the tree was edited
    // between frames, so verify before dereferencing either of them.
    if (press_target && !contains_widget(roots, press_target)) press_target = nullptr;
    if (focus_target && !contains_widget(roots, focus_target)) {
        g_keyboard_focus_count = std::max(0, g_keyboard_focus_count - 1);
        focus_target = nullptr;
    }

    UIWidget* hovered = nullptr;
    for (auto it = roots.rbegin(); it != roots.rend() && !hovered; ++it) {
        if (*it) hovered = hit_test(**it, input.mouse_x, input.mouse_y);
    }

    if (hovered) {
        hovered->hovered = true;
        mouse_consumed = true;
    }

    if (!input.interactive) {
        // Preview mode: the layout is drawn, but the mouse belongs to the editor.
        press_target = nullptr;
        return;
    }

    if (input.mouse_pressed) {
        press_target = hovered;

        // Focus follows the click, so clicking anywhere else commits a text field.
        UIWidget* new_focus = (hovered && hovered->type == UIWidget::Widget_TextField) ? hovered : nullptr;
        if (new_focus != focus_target) {
            if (focus_target) {
                focus_target->focused = false;
                g_keyboard_focus_count = std::max(0, g_keyboard_focus_count - 1);
            }
            focus_target = new_focus;
            if (focus_target) {
                focus_target->focused = true;
                ++g_keyboard_focus_count;
            }
        }
    }

    // Sliders track the pointer for as long as the button is held, even once it has
    // been dragged outside the widget - releasing the handle the moment the cursor
    // slips off the track is the single most irritating slider bug there is.
    if (press_target && press_target->type == UIWidget::Widget_Slider && input.mouse_down) {
        const UIRect& r = press_target->computed_rect;
        const float track_left = r.x + press_target->padding;
        const float track_width = std::max(1.0f, r.width - 2.0f * press_target->padding);
        const float t = clamp01((input.mouse_x - track_left) / track_width);
        const float before = press_target->value;
        press_target->set_fraction(t);
        if (std::abs(press_target->value - before) > 1e-6f) {
            press_target->value_changed = true;
            changed_widgets.push_back(press_target->name);
        }
        mouse_consumed = true;
    }

    if (press_target) {
        press_target->pressed = input.mouse_down;
        mouse_consumed = true;
    }

    if (input.mouse_released) {
        // A click only counts when it is released over the widget it started on.
        if (press_target && press_target == hovered) {
            if (press_target->type == UIWidget::Widget_Checkbox) {
                press_target->value = (press_target->fraction() > 0.5f) ? press_target->min_value
                                                                       : press_target->max_value;
                press_target->value_changed = true;
                changed_widgets.push_back(press_target->name);
            }
            press_target->clicked = true;
            clicked_widgets.push_back(press_target->name);
        }
        if (press_target) press_target->pressed = false;
        press_target = nullptr;
    }

    // Text entry for the focused field. The input queue is read rather than
    // consumed: clearing it here would swallow characters ImGui itself is due to
    // deliver to an editor widget in the same frame.
    if (focus_target && focus_target->type == UIWidget::Widget_TextField) {
        ImGuiIO& io = ImGui::GetIO();
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
            const unsigned int c = static_cast<unsigned int>(io.InputQueueCharacters[i]);
            if (c < 32 || c == 127) continue; // control characters are handled below
            if (c < 128) {
                focus_target->text.push_back(static_cast<char>(c));
                focus_target->value_changed = true;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !focus_target->text.empty()) {
            // Step back over a whole UTF-8 sequence so a multi-byte character is not
            // left as a truncated, invalid tail.
            size_t cut = focus_target->text.size() - 1;
            while (cut > 0 && (static_cast<unsigned char>(focus_target->text[cut]) & 0xC0) == 0x80) --cut;
            focus_target->text.erase(cut);
            focus_target->value_changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            focus_target->focused = false;
            g_keyboard_focus_count = std::max(0, g_keyboard_focus_count - 1);
            focus_target->clicked = true;
            clicked_widgets.push_back(focus_target->name);
            focus_target = nullptr;
        }
        if (focus_target && focus_target->value_changed) {
            changed_widgets.push_back(focus_target->name);
        }
    }
}

// --- Drawing ---------------------------------------------------------------

void UICanvasComponent::draw_widget(const UIWidget& widget, void* draw_list_ptr,
                                    float scale, const UIRect& screen_rect) {
    if (!widget.visible) return;

    ImDrawList* dl = static_cast<ImDrawList*>(draw_list_ptr);
    const UIRect& r = widget.computed_rect;

    const ImVec2 p_min(screen_rect.x + r.x * scale, screen_rect.y + r.y * scale);
    const ImVec2 p_max(screen_rect.x + (r.x + r.width) * scale,
                       screen_rect.y + (r.y + r.height) * scale);
    const float rounding = widget.corner_radius * scale;

    // Interactive widgets swap their background for the hover / pressed / disabled
    // colour. Everything else keeps its own, so a Panel is not tinted by a cursor
    // that merely passed over it.
    Vector4 background = widget.background_color;
    if (UIWidget::type_is_interactive(widget.type)) {
        if (!widget.interactive) background = widget.disabled_color;
        else if (widget.pressed)  background = widget.pressed_color;
        else if (widget.hovered)  background = widget.hover_color;
    }

    if (background.w > 0.001f) {
        dl->AddRectFilled(p_min, p_max, to_col32(background), rounding, ImDrawFlags_RoundCornersAll);
    }

    // Fill, drawn between the background and the border so a bar's fill is clipped
    // by the same rounded outline as its track.
    if (widget.type == UIWidget::Widget_ProgressBar || widget.type == UIWidget::Widget_Slider) {
        const float inset = widget.padding * scale;
        const float track_left = p_min.x + inset;
        const float track_right = p_max.x - inset;
        const float fill_right = track_left + std::max(0.0f, track_right - track_left) * widget.fraction();

        if (widget.type == UIWidget::Widget_ProgressBar) {
            if (fill_right > track_left) {
                dl->AddRectFilled(ImVec2(track_left, p_min.y + inset),
                                  ImVec2(fill_right, p_max.y - inset),
                                  to_col32(widget.fill_color),
                                  std::max(0.0f, rounding - inset), ImDrawFlags_RoundCornersAll);
            }
        } else {
            // Slider: a thin track with a round handle, which reads as draggable in a
            // way a filled bar does not.
            const float mid_y = (p_min.y + p_max.y) * 0.5f;
            const float track_h = std::max(2.0f, 4.0f * scale);
            dl->AddRectFilled(ImVec2(track_left, mid_y - track_h * 0.5f),
                              ImVec2(track_right, mid_y + track_h * 0.5f),
                              to_col32(widget.border_color), track_h * 0.5f);
            if (fill_right > track_left) {
                dl->AddRectFilled(ImVec2(track_left, mid_y - track_h * 0.5f),
                                  ImVec2(fill_right, mid_y + track_h * 0.5f),
                                  to_col32(widget.fill_color), track_h * 0.5f);
            }
            const float handle_r = std::max(4.0f, (r.height * 0.35f) * scale);
            dl->AddCircleFilled(ImVec2(fill_right, mid_y), handle_r, to_col32(widget.fill_color), 20);
            dl->AddCircle(ImVec2(fill_right, mid_y), handle_r, to_col32(widget.text_color), 20, 1.5f);
        }
    }

    if (widget.type == UIWidget::Widget_Image && !widget.image_path.empty()) {
        auto texture = ResourceManager::get().load_async<TextureResource>(widget.image_path);
        // Only once the upload has actually happened: drawing id 0 paints an opaque
        // white rectangle over whatever is behind it while the texture streams in.
        if (texture && texture->get_state() == ResourceState::LoadedGPU && texture->get_texture_id() != 0) {
            dl->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(
                                    static_cast<intptr_t>(texture->get_texture_id()))),
                                p_min, p_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                to_col32(widget.image_tint), rounding, ImDrawFlags_RoundCornersAll);
        }
    }

    // Checkbox mark, drawn to the left of its label.
    float text_left_inset = 0.0f;
    if (widget.type == UIWidget::Widget_Checkbox) {
        const float box = std::min(r.height - widget.padding * 2.0f, 20.0f) * scale;
        const float bx = p_min.x + widget.padding * scale;
        const float by = (p_min.y + p_max.y) * 0.5f - box * 0.5f;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + box, by + box),
                          to_col32(widget.pressed_color), 3.0f * scale);
        dl->AddRect(ImVec2(bx, by), ImVec2(bx + box, by + box),
                    to_col32(widget.border_color), 3.0f * scale, 0, std::max(1.0f, scale));
        if (widget.fraction() > 0.5f) {
            const float pad = box * 0.24f;
            dl->AddLine(ImVec2(bx + pad, by + box * 0.52f),
                        ImVec2(bx + box * 0.44f, by + box - pad),
                        to_col32(widget.fill_color), std::max(1.5f, 2.0f * scale));
            dl->AddLine(ImVec2(bx + box * 0.44f, by + box - pad),
                        ImVec2(bx + box - pad, by + pad),
                        to_col32(widget.fill_color), std::max(1.5f, 2.0f * scale));
        }
        text_left_inset = box + widget.padding * scale;
    }

    if (widget.border_thickness > 0.001f && widget.border_color.w > 0.001f) {
        dl->AddRect(p_min, p_max, to_col32(widget.border_color), rounding,
                    ImDrawFlags_RoundCornersAll, widget.border_thickness * scale);
    }

    if (UIWidget::type_has_text(widget.type) && !widget.text.empty()) {
        ImFont* font = ImGui::GetFont();
        const float font_px = std::max(1.0f, kBaseFontSize * widget.font_scale * scale);
        const float pad = widget.padding * scale;

        const float text_area_left = p_min.x + pad + text_left_inset;
        const float text_area_right = p_max.x - pad;
        const float wrap_width = widget.word_wrap ? std::max(1.0f, text_area_right - text_area_left) : 0.0f;

        const ImVec2 size = font->CalcTextSizeA(font_px, FLT_MAX, wrap_width, widget.text.c_str());

        float tx = text_area_left;
        if (widget.h_align == UIWidget::Align_Center) {
            tx = (text_area_left + text_area_right) * 0.5f - size.x * 0.5f;
        } else if (widget.h_align == UIWidget::Align_End) {
            tx = text_area_right - size.x;
        }

        float ty = p_min.y + pad;
        if (widget.v_align == UIWidget::Align_Center) {
            ty = (p_min.y + p_max.y) * 0.5f - size.y * 0.5f;
        } else if (widget.v_align == UIWidget::Align_End) {
            ty = p_max.y - pad - size.y;
        }

        // Clipped to the widget so a long string cannot spill across the screen.
        dl->PushClipRect(p_min, p_max, true);
        dl->AddText(font, font_px, ImVec2(tx, ty), to_col32(widget.text_color),
                    widget.text.c_str(), nullptr, wrap_width);
        dl->PopClipRect();
    }

    // A caret, so a focused field looks focused. Blink is driven off ImGui's clock
    // rather than a member, which keeps the widget free of frame state.
    if (widget.focused && widget.type == UIWidget::Widget_TextField) {
        if (std::fmod(static_cast<float>(ImGui::GetTime()), 1.0f) < 0.5f) {
            ImFont* font = ImGui::GetFont();
            const float font_px = std::max(1.0f, kBaseFontSize * widget.font_scale * scale);
            const ImVec2 size = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, widget.text.c_str());
            const float pad = widget.padding * scale;
            float cx = p_min.x + pad + size.x;
            if (widget.h_align == UIWidget::Align_Center) cx = (p_min.x + p_max.x) * 0.5f + size.x * 0.5f;
            else if (widget.h_align == UIWidget::Align_End) cx = p_max.x - pad;
            const float cy0 = (p_min.y + p_max.y) * 0.5f - font_px * 0.5f;
            dl->AddLine(ImVec2(cx + 1.0f, cy0), ImVec2(cx + 1.0f, cy0 + font_px),
                        to_col32(widget.text_color), std::max(1.0f, scale));
        }
    }

    for (const auto& child : widget.children) {
        if (child) draw_widget(*child, draw_list_ptr, scale, screen_rect);
    }
}

// --- Frame -----------------------------------------------------------------

void UICanvasComponent::render(const UIRect& screen_rect, const UIInputState& input_in) {
    clear_events();
    mouse_consumed = false;

    if (!visible || screen_rect.width <= 1.0f || screen_rect.height <= 1.0f) return;

    // Reference-resolution scaling. The geometric blend of the two axis ratios is
    // what keeps a HUD authored for 16:9 sensible on an ultrawide: matching width
    // alone makes it grow off the bottom of the screen, height alone leaves it
    // stranded in the middle.
    float scale = 1.0f;
    if (scale_mode == Scale_WithScreenSize &&
        reference_resolution.x > 1.0f && reference_resolution.y > 1.0f) {
        const float sw = screen_rect.width / reference_resolution.x;
        const float sh = screen_rect.height / reference_resolution.y;
        const float match = clamp01(match_width_or_height);
        scale = std::pow(sw, 1.0f - match) * std::pow(sh, match);
    }
    if (!(scale > 0.0001f)) scale = 1.0f;

    const UIRect root_rect{ 0.0f, 0.0f, screen_rect.width / scale, screen_rect.height / scale };

    for (auto& widget : roots) {
        if (widget) layout(*widget, root_rect);
    }

    // The pointer is converted into the same logical space the layout is in, so hit
    // testing compares like with like whatever the scale turned out to be.
    UIInputState input = input_in;
    input.mouse_x = (input_in.mouse_x - screen_rect.x) / scale;
    input.mouse_y = (input_in.mouse_y - screen_rect.y) / scale;

    update_interaction(input);

    // The foreground list draws over the viewport in the editor and over the
    // fullscreen game image in a standalone build, without either having to know
    // this exists.
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (const auto& widget : roots) {
        if (widget) draw_widget(*widget, dl, scale, screen_rect);
    }
}
