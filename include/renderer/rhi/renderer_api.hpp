#pragma once

#include "rhi_command_list.hpp"
#include <memory>
#include <string>

namespace RHI {

enum class BackendAPI {
    None = 0,
    OpenGL,
    Vulkan,
    DirectX12
};

// Global interface for the hardware rendering context
class RendererAPI {
public:
    virtual ~RendererAPI() = default;

    virtual bool init(uint32_t width, uint32_t height) = 0;
    virtual void shutdown() = 0;

    virtual void begin_frame() = 0;
    virtual void end_frame() = 0;

    // Factory method for creating command lists
    virtual std::unique_ptr<RHICommandList> create_command_list() = 0;
    
    // Virtual methods for resource creation (buffers, shaders) will go here
    // e.g. virtual uint32_t create_vertex_buffer(float* vertices, uint32_t size) = 0;

    static BackendAPI current_api;
    static BackendAPI get_api() { return current_api; }
    static std::unique_ptr<RendererAPI> create(); // Factory for the current backend

};

} // namespace RHI
