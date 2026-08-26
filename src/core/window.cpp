#include "core/window.hpp"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <SDL2/SDL_opengl.h>
#include <iostream>
#include <filesystem>

Window::Window(const std::string& title, int width, int height)
    : title(title), width(width), height(height) {}

Window::~Window() {
    shutdown();
}

bool Window::initialize() {
    // GAMECONTROLLER so the input map can bind pad buttons and sticks.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) < 0) {
        std::cerr << "Failed to initialize SDL: " << SDL_GetError() << std::endl;
        return false;
    }

    // Set GL attributes (Downgraded to 3.3 for maximum compatibility)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    // MSAA is now handled via FBOs in modern deferred/forward renderers
    // SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    // SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    sdl_window = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width,
        height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    if (!sdl_window) {
        std::cerr << "Failed to create SDL window: " << SDL_GetError() << std::endl;
        return false;
    }

    gl_context = SDL_GL_CreateContext(sdl_window);
    if (!gl_context) {
        std::cerr << "Failed to create OpenGL context: " << SDL_GetError() << std::endl;
        return false;
    }

    // Enable MSAA in OpenGL (Disabled due to Xvfb GLXBadFBConfig)
    // glEnable(GL_MULTISAMPLE);

    // Enable VSync
    SDL_GL_SetSwapInterval(1);

    return true;
}

#include "core/ubuntu_ttf.h"
#include "core/roboto_ttf.h"

void Window::init_imgui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Load fonts
    ImGuiIO& io = ImGui::GetIO();

    // Turn a plain click-release on a Drag/Slider widget into a text edit. By default
    // ImGui requires ctrl+click or a double-click to type a number, which is why
    // entering a value took several attempts.
    io.ConfigDragClickToInputText = true;

    
    // Create copies of the font data for ImGui
    void* ubuntu_data = malloc(Ubuntu_Regular_ttf_len);
    memcpy(ubuntu_data, Ubuntu_Regular_ttf, Ubuntu_Regular_ttf_len);
    io.Fonts->AddFontFromMemoryTTF(ubuntu_data, Ubuntu_Regular_ttf_len, 18.0f);
    
    void* roboto_data = malloc(Roboto_Regular_ttf_len);
    memcpy(roboto_data, Roboto_Regular_ttf, Roboto_Regular_ttf_len);
    io.Fonts->AddFontFromMemoryTTF(roboto_data, Roboto_Regular_ttf_len, 16.0f);

    ImGui_ImplSDL2_InitForOpenGL(sdl_window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 450");
    imgui_initialized = true;
}

void Window::new_frame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void Window::render_imgui() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Window::shutdown() {
    if (imgui_initialized) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        imgui_initialized = false;
    }

    if (gl_context) {
        SDL_GL_DeleteContext(gl_context);
        gl_context = nullptr;
    }
    if (sdl_window) {
        SDL_DestroyWindow(sdl_window);
        sdl_window = nullptr;
    }
    SDL_Quit();
}

void Window::poll_events(bool& running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT) {
            running = false;
        } else if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                width = event.window.data1;
                height = event.window.data2;
                glViewport(0, 0, width, height);
            }
        }
    }
}

void Window::swap_buffers() {
    if (sdl_window) {
        SDL_GL_SwapWindow(sdl_window);
    }
}
