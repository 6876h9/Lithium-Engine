#include "renderer/rhi/vulkan/vulkan_renderer_api.hpp"
#include <iostream>

namespace RHI {

bool VulkanRendererAPI::init(uint32_t width, uint32_t height) {
    std::cout << "[Vulkan] Initializing Vulkan Backend (STUB)..." << std::endl;
    return true;
}

void VulkanRendererAPI::shutdown() {
    std::cout << "[Vulkan] Shutting down Vulkan Backend (STUB)..." << std::endl;
}

void VulkanRendererAPI::begin_frame() {
    // Stub
}

void VulkanRendererAPI::end_frame() {
    // Stub
}

std::unique_ptr<RHICommandList> VulkanRendererAPI::create_command_list() {
    return nullptr;
}

} // namespace RHI
