#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>

namespace Rendering {

// Virtual Texturing (MegaTexture) architecture.
// Instead of loading huge 8K/16K textures into VRAM, we break them into small tiles (e.g., 128x128).
// The GPU reads a Page Table texture to find where a specific tile lives in a Physical Memory Pool.

struct VirtualTexturePage {
    uint32_t virtual_x;
    uint32_t virtual_y;
    uint32_t mip_level;
    uint32_t physical_x;
    uint32_t physical_y;
    bool is_resident;
};

class VirtualTextureSystem {
public:
    VirtualTextureSystem(uint32_t page_size = 128, uint32_t pool_size_pages = 1024) 
        : page_size(page_size), pool_size_pages(pool_size_pages) {}

    // Initialize physical texture pool (e.g. 1024 pages of 128x128 = 4096x4096 physical texture)
    void init_physical_pool() {
        // RHI call to create physical pool texture
    }

    // Read back feedback buffer from GPU to determine which pages are currently visible on screen
    void process_feedback_buffer(const std::vector<uint32_t>& feedback_data) {
        // Decode feedback (virtual_id, mip) and queue for loading
    }

    // Load missing pages from disk (in background thread) and upload to physical pool
    void upload_pending_pages() {
        // Stream data to physical pool
        // Update page table texture
    }

private:
    uint32_t page_size;
    uint32_t pool_size_pages;
    
    // Maps a virtual ID to its page mapping
    std::unordered_map<uint32_t, VirtualTexturePage> page_table;
};

} // namespace Rendering
