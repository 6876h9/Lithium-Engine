#include "renderer/rhi/renderer_api.hpp"
#include "renderer/rhi/opengl/opengl_renderer_api.hpp"
#include "renderer/rhi/vulkan/vulkan_renderer_api.hpp"

namespace RHI {

BackendAPI RendererAPI::current_api = BackendAPI::OpenGL;

std::unique_ptr<RendererAPI> RendererAPI::create() {
    switch (current_api) {
        case BackendAPI::OpenGL: return std::make_unique<OpenGLRendererAPI>();
        case BackendAPI::Vulkan: return std::make_unique<VulkanRendererAPI>();
        case BackendAPI::DirectX12:
        case BackendAPI::None:
        default: return nullptr;
    }
}

} // namespace RHI
