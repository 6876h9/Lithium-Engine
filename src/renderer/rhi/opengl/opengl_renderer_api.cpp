#include "renderer/rhi/opengl/opengl_renderer_api.hpp"
#include "renderer/gl_loader.hpp"
#include <iostream>

namespace RHI {

// --- OpenGLCommandList ---

void OpenGLCommandList::begin() {
    // For raw OpenGL, typically no explicit command list recording unless using specific extensions
}

void OpenGLCommandList::end() {
    // End recording
}

void OpenGLCommandList::set_viewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    glViewport(x, y, width, height);
}

void OpenGLCommandList::set_clear_color(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
}

void OpenGLCommandList::clear(bool color, bool depth, bool stencil) {
    GLbitfield mask = 0;
    if (color) mask |= GL_COLOR_BUFFER_BIT;
    if (depth) mask |= GL_DEPTH_BUFFER_BIT;
    if (stencil) mask |= GL_STENCIL_BUFFER_BIT;
    if (mask != 0) glClear(mask);
}

void OpenGLCommandList::draw_indexed(uint32_t index_count, uint32_t start_index, uint32_t base_vertex) {
    // glDrawElementsBaseVertex(GL_TRIANGLES, index_count, GL_UNSIGNED_INT, (void*)(start_index * sizeof(uint32_t)), base_vertex);
}

void OpenGLCommandList::draw_instanced(uint32_t vertex_count, uint32_t instance_count, uint32_t start_vertex, uint32_t start_instance) {
    // glDrawArraysInstancedBaseInstance(GL_TRIANGLES, start_vertex, vertex_count, instance_count, start_instance);
}

void OpenGLCommandList::dispatch_compute(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) {
    if (glDispatchCompute) {
        glDispatchCompute(group_count_x, group_count_y, group_count_z);
    }
}

// --- OpenGLRendererAPI ---

bool OpenGLRendererAPI::init(uint32_t width, uint32_t height) {
    std::cout << "[RHI] Initializing OpenGL Renderer API" << std::endl;
    // OpenGL initialization is mostly handled by SDL in the Window class for now,
    // but we can set up global states here.
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    return true;
}

void OpenGLRendererAPI::shutdown() {
    std::cout << "[RHI] Shutting down OpenGL Renderer API" << std::endl;
}

void OpenGLRendererAPI::begin_frame() {
    // Set up state for new frame
}

void OpenGLRendererAPI::end_frame() {
    // Flush/Swap logic (often handled by SDL_GL_SwapWindow in Engine)
}

std::unique_ptr<RHICommandList> OpenGLRendererAPI::create_command_list() {
    return std::make_unique<OpenGLCommandList>();
}

} // namespace RHI
