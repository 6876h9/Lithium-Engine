#pragma once

#include "world/actor.hpp"
#include "world/static_mesh_component.hpp"
#include <string>

class SpriteActor : public Actor {
public:
    SpriteActor(const std::string& name);
    virtual ~SpriteActor() = default;

    virtual void begin_play() override;
    virtual void tick(float delta_time) override;

    // Sprite specific properties
    void set_texture(const std::string& texture_path);
    std::string get_texture() const { return current_texture; }

private:
    StaticMeshComponent* mesh_component = nullptr;
    std::string current_texture = "";
};
