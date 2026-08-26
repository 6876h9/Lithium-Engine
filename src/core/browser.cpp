#include "core/browser.hpp"
#include "imgui.h"
#include <iostream>

ProjectBrowser::ProjectBrowser() {}

ProjectBrowser::~ProjectBrowser() {}

bool ProjectBrowser::render(EngineConfig& out_config) {
    static bool theme_applied = false;
    if (!theme_applied) {
        apply_dark_theme();
        theme_applied = true;
    }
    bool launched = false;

    // Center the project browser window and give it a fixed, professional size
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 window_size(900, 600);
    ImVec2 window_pos(
        viewport->WorkPos.x + (viewport->WorkSize.x - window_size.x) * 0.5f,
        viewport->WorkPos.y + (viewport->WorkSize.y - window_size.y) * 0.5f
    );
    
    ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
    
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;

    ImGui::Begin("Lithium Engine Launcher & Project Browser", nullptr, window_flags);

    // Title / Header
    ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.0f, 1.0f), "LITHIUM ENGINE LAUNCHER");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    // Left Column: Templates selection
    ImGui::BeginChild("Templates", ImVec2(ImGui::GetContentRegionAvail().x * 0.4f, ImGui::GetContentRegionAvail().y - 80.0f), true);
    ImGui::Text("Select a Project Template:");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 5.0f));

    auto render_template_item = [&](EngineTemplate t, const char* label, const char* description) {
        bool selected = (out_config.selected_template == t);
        if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_None, ImVec2(0, 50))) {
            out_config.selected_template = t;
        }
        ImGui::TextDisabled("%s", description);
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
    };

    render_template_item(EngineTemplate::Blank, "Blank Project", "A clean start with no boilerplate. Ideal for custom setups.");
    render_template_item(EngineTemplate::FPS, "First-Person Shooter (FPS)", "Equipped with a basic first-person camera, player movement, and gun rig.");
    render_template_item(EngineTemplate::ThirdPerson, "Third-Person Action", "Includes a camera spring-arm, character movement, and mesh placement.");
    render_template_item(EngineTemplate::Platformer, "2D Side-Scroller", "Optimized physics and camera for a classic side-scrolling platformer.");
    render_template_item(EngineTemplate::SSRShowcase, "Reflections Showcase (SSR)", "Night scene: a floating cube over a polished mirror floor, lit by a hanging bulb. Built to show off screen-space reflections.");

    ImGui::EndChild();

    ImGui::SameLine();

    // Right Column: Advanced Configuration & Target Options
    ImGui::BeginChild("Settings", ImVec2(0, ImGui::GetContentRegionAvail().y - 80.0f), true);
    ImGui::Text("Advanced Project Settings:");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    // Project metadata
    static char proj_name[128] = "MyAntigravityGame";
    static char proj_path[256] = "/home/burair1991/projects";
    
    ImGui::InputText("Project Name", proj_name, IM_ARRAYSIZE(proj_name));
    ImGui::InputText("Project Path", proj_path, IM_ARRAYSIZE(proj_path));
    
    out_config.project_name = proj_name;
    out_config.project_path = proj_path;

    ImGui::Dummy(ImVec2(0.0f, 15.0f));
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Graphics & Display");
    ImGui::Separator();

    // Resolution dropdown
    static const char* resolutions[] = {
        "320x200", "320x240", "400x300", "480x320", "512x384", "640x360", "640x400", "640x480", "720x480", "720x576", 
        "800x600", "848x480", "854x480", "960x540", "1024x576", "1024x600", "1024x768", "1152x768", "1152x864", "1280x720", 
        "1280x768", "1280x800", "1280x960", "1280x1024", "1360x768", "1366x768", "1400x1050", "1440x900", "1440x1080", 
        "1600x900", "1600x1200", "1680x1050", "1920x1080", "1920x1200", "1920x1440", "2048x1080", "2048x1152", "2048x1536", 
        "2560x1080", "2560x1440", "2560x1600", "2560x2048", "2880x1800", "3200x1800", "3440x1440", "3840x1080", "3840x1600", 
        "3840x2160", "3840x2400", "4096x2160", "5120x1440", "5120x2160", "5120x2880", "7680x2160", "7680x4320"
    };
    static int current_resolution_idx = 32; // Default to 1920x1080
    if (ImGui::Combo("Screen Resolution", &current_resolution_idx, resolutions, IM_ARRAYSIZE(resolutions))) {
        sscanf(resolutions[current_resolution_idx], "%dx%d", &out_config.resolution_width, &out_config.resolution_height);
    } else {
        // Ensure out_config gets initialized correctly initially
        sscanf(resolutions[current_resolution_idx], "%dx%d", &out_config.resolution_width, &out_config.resolution_height);
    }

    ImGui::Checkbox("Enable VSync (Limit Framerate to Monitor)", &out_config.vsync);

    // Advanced UE4 features checkboxes
    ImGui::Checkbox("Enable Ray Tracing & Path Tracing (RTX/DirectX Raytracing Sim)", &out_config.enable_raytracing);
    ImGui::Checkbox("Enable NVIDIA DLSS & AMD FSR Super Resolution (Upscaling)", &out_config.enable_dlss_fsr);
    ImGui::Checkbox("Enable UE4 High-Fidelity Lighting Shader (Directional Sun)", &out_config.enable_ue4_lighting);
    
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.0f, 1.0f), "Game Mode");
    ImGui::Separator();
    ImGui::Checkbox("2D Mode (Orthographic Camera & Sprite Support)", &out_config.is_2d_mode);

    // AA Samples
    const char* aa_options[] = { "Off", "2x MSAA", "4x MSAA", "8x MSAA" };
    static int aa_idx = 2; // 4x MSAA default
    if (ImGui::Combo("Anti-Aliasing", &aa_idx, aa_options, IM_ARRAYSIZE(aa_options))) {
        if (aa_idx == 0) out_config.antialiasing_samples = 1;
        else if (aa_idx == 1) out_config.antialiasing_samples = 2;
        else if (aa_idx == 2) out_config.antialiasing_samples = 4;
        else if (aa_idx == 3) out_config.antialiasing_samples = 8;
    }

    ImGui::Dummy(ImVec2(0.0f, 15.0f));
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Physics & Subsystems");
    ImGui::Separator();

    // Physics Hz
    ImGui::SliderInt("Physics Tick Rate (Hz)", &out_config.physics_hz, 30, 240);

    // Platform Target
    const char* platforms[] = { "Desktop (Linux/Windows/macOS)", "Console Target", "Mobile / Web Assembly" };
    if (ImGui::Combo("Target Hardware", &current_platform_idx, platforms, IM_ARRAYSIZE(platforms))) {
        out_config.target_platform = platforms[current_platform_idx];
    }

    ImGui::EndChild();

    // Bottom Area: Action Buttons
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    // Align to the right
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x - 220.0f, 0.0f));
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.4f, 0.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.5f, 0.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.3f, 0.0f, 1.0f));

    if (ImGui::Button("Create & Launch Project", ImVec2(200, 45))) {
        launched = true;
    }

    ImGui::PopStyleColor(3);

    ImGui::End();

    return launched;
}

void ProjectBrowser::apply_dark_theme() {
    // Unreal Engine 5 inspired Slate Theme styling
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 4.0f;

    // Slate Dark Colors
    colors[ImGuiCol_Text]                   = ImVec4(0.85f, 0.87f, 0.89f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.09f, 0.10f, 0.11f, 1.00f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.12f, 0.13f, 0.14f, 0.94f);
    colors[ImGuiCol_Border]                 = ImVec4(0.20f, 0.22f, 0.24f, 1.00f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.16f, 0.17f, 0.19f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.24f, 0.25f, 0.27f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.30f, 0.31f, 0.33f, 1.00f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.09f, 0.10f, 0.11f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.09f, 0.10f, 0.11f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.10f, 0.10f, 0.10f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
    colors[ImGuiCol_CheckMark]              = ImVec4(0.85f, 0.45f, 0.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.85f, 0.45f, 0.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]        = ImVec4(1.00f, 0.55f, 0.10f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.18f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.26f, 0.28f, 0.30f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.35f, 0.37f, 0.40f, 1.00f);
    colors[ImGuiCol_Header]                 = ImVec4(0.20f, 0.22f, 0.24f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.85f, 0.45f, 0.00f, 0.40f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.85f, 0.45f, 0.00f, 0.80f);
    colors[ImGuiCol_Separator]              = ImVec4(0.20f, 0.22f, 0.24f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.85f, 0.45f, 0.00f, 0.78f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.85f, 0.45f, 0.00f, 1.00f);
}
