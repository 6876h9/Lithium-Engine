#pragma once

#include <string>
#include <SDL2/SDL.h>

class Window {
public:
    Window(const std::string& title, int width, int height);
    ~Window();

    bool initialize();
    void shutdown();

    void poll_events(bool& running);
    void swap_buffers();

    // ImGui integration hooks
    void init_imgui();
    void new_frame();
    void render_imgui();

    SDL_Window* get_sdl_window() const { return sdl_window; }
    SDL_GLContext get_gl_context() const { return gl_context; }
    int get_width() const { return width; }
    int get_height() const { return height; }

private:
    std::string title;
    int width;
    int height;
    bool imgui_initialized = false;
    SDL_Window* sdl_window = nullptr;
    SDL_GLContext gl_context = nullptr;
};
