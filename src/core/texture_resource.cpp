#include "core/texture_resource.hpp"
#include "stb_image.h"
#include "renderer/gl_loader.hpp"
#include <iostream>

TextureResource::TextureResource(const std::string& filepath) : Resource(filepath) {}

TextureResource::~TextureResource() {
    if (cpu_data) {
        stbi_image_free(cpu_data);
    }
    if (texture_id != 0) {
        glDeleteTextures(1, &texture_id);
    }
}

bool TextureResource::load_from_disk() {
    stbi_set_flip_vertically_on_load(true);
    cpu_data = stbi_load(filepath.c_str(), &width, &height, &channels, 4);
    if (!cpu_data) {
        std::cerr << "[ResourceManager] Failed to load texture from disk: " << filepath << std::endl;
        return false;
    }
    return true;
}

bool TextureResource::upload_to_gpu() {
    if (!cpu_data) return false;

    // Hot reloading calls this again on an already-uploaded resource. Release the
    // previous texture first, otherwise every reload leaks a full texture on the GPU.
    if (texture_id != 0) {
        glDeleteTextures(1, &texture_id);
        texture_id = 0;
    }

    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    // Default wrapping and filtering
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // We loaded with 4 channels requested from stb_image
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, cpu_data);
    glGenerateMipmap(GL_TEXTURE_2D);

    stbi_image_free(cpu_data);
    cpu_data = nullptr;

    return true;
}
