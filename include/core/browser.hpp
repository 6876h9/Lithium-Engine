#pragma once

#include <string>
#include <vector>

enum class EngineTemplate {
    Blank,
    FPS,
    ThirdPerson,
    Platformer,
    SSRShowcase
};

struct EngineConfig {
    std::string project_name = "MyCustomProject";
    std::string project_path = "/home/burair1991/projects";
    EngineTemplate selected_template = EngineTemplate::Blank;
    
    // Advanced settings
    int resolution_width = 1280;
    int resolution_height = 720;
    bool vsync = true;
    int antialiasing_samples = 4; // 1 (none), 2, 4, 8
    int physics_hz = 60;
    std::string target_platform = "Desktop";

    // New UE4 Advanced features
    bool enable_dlss_fsr = true;
    bool enable_raytracing = true;
    bool enable_ue4_lighting = true;
    bool is_2d_mode = false;

    // --- Options (persisted in engine_config.json) ---
    bool fullscreen = false;
    float master_volume = 1.0f;
    bool enable_ssr = true;
    bool enable_bloom = true;
    // Strength of ambient occlusion on indirect light. 1 is full effect.
    float ssao_strength = 1.0f;
    bool enable_taa_option = true;
    float field_of_view = 45.0f;
    // Chosen environment map, relative to the executable. Empty means the bundled
    // default. Persisted so a picked sky survives a restart.
    std::string sky_hdri = "";

    // --- Developer options ---
    // These exist to make a change testable in one launch instead of a manual
    // click-through every time: jump straight past project setup, start the camera
    // framed on whatever is being worked on, and optionally spawn a subject actor
    // automatically.
    bool dev_auto_enter_editor = false;
    float dev_camera_x = 0.0f;
    float dev_camera_y = 0.0f;
    float dev_camera_z = 0.0f;
    float dev_camera_yaw = 0.0f;
    float dev_camera_pitch = 0.0f;
    int dev_spawn_on_start = 0;   // 0 none, 1 Cube, 2 Sphere, 3 Static Light Ray
    bool dev_show_startup_timings = false;
};

class ProjectBrowser {
public:
    ProjectBrowser();
    ~ProjectBrowser();

    // Renders the ImGui interface. Returns true if the user clicked "Launch Engine/Create Project"
    bool render(EngineConfig& out_config);

private:
    void apply_dark_theme();
    int current_resolution_idx = 1; // Default to 1280x720
    int current_platform_idx = 0;   // Default to Desktop
};
