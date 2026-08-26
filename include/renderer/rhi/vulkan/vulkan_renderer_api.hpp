#pragma once
#include "renderer/rhi/renderer_api.hpp"

namespace RHI {
    class VulkanRendererAPI : public RendererAPI {
    public:
        VulkanRendererAPI() = default;
        virtual ~VulkanRendererAPI() = default;
        virtual bool init(uint32_t width, uint32_t height) override;
        virtual void shutdown() override;
        virtual void begin_frame() override;
        virtual void end_frame() override;
        virtual std::unique_ptr<RHICommandList> create_command_list() override;
    };
}
