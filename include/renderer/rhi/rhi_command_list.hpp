#pragma once

#include <cstdint>

namespace RHI {

enum class PrimitiveTopology {
    Triangles,
    Lines,
    Points
};

// Represents a buffer of commands to be executed on the GPU
// This abstracts away direct OpenGL/Vulkan draw calls.
class RHICommandList {
public:
    virtual ~RHICommandList() = default;

    virtual void begin() = 0;
    virtual void end() = 0;

    virtual void set_viewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;
    virtual void set_clear_color(float r, float g, float b, float a) = 0;
    virtual void clear(bool color, bool depth, bool stencil) = 0;

    virtual void draw_indexed(uint32_t index_count, uint32_t start_index, uint32_t base_vertex) = 0;
    virtual void draw_instanced(uint32_t vertex_count, uint32_t instance_count, uint32_t start_vertex, uint32_t start_instance) = 0;
    
    // Compute operations
    virtual void dispatch_compute(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) = 0;
};

} // namespace RHI
