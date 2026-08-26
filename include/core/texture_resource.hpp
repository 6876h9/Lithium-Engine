#pragma once

#include "core/resource.hpp"

class TextureResource : public Resource {
public:
    TextureResource(const std::string& filepath);
    virtual ~TextureResource();

    virtual bool load_from_disk() override;
    virtual bool upload_to_gpu() override;

    unsigned int get_texture_id() const { return texture_id; }

private:
    unsigned char* cpu_data = nullptr;
    int width = 0;
    int height = 0;
    int channels = 0;

    unsigned int texture_id = 0;
};
