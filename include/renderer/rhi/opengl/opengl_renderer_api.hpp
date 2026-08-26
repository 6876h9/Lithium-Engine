#pragma once

#include "renderer/rhi/renderer_api.hpp"
#include <SDL2/SDL_opengl.h>

namespace RHI {

class OpenGLCommandList : public RHICommandList {
public:
    OpenGLCommandList() = default;
    ~OpenGLCommandList() override = default;

    void begin() override;
    void end() override;

    void set_viewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
    void set_clear_color(float r, float g, float b, float a) override;
    void clear(bool color, bool depth, bool stencil) override;

    void draw_indexed(uint32_t index_count, uint32_t start_index, uint32_t base_vertex) override;
    void draw_instanced(uint32_t vertex_count, uint32_t instance_count, uint32_t start_vertex, uint32_t start_instance) override;
    
    void dispatch_compute(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) override;
};

class OpenGLRendererAPI : public RendererAPI {
public:
    OpenGLRendererAPI() = default;
    ~OpenGLRendererAPI() override = default;

    bool init(uint32_t width, uint32_t height) override;
    void shutdown() override;

    void begin_frame() override;
    void end_frame() override;

    std::unique_ptr<RHICommandList> create_command_list() override;
};

} // namespace RHI
