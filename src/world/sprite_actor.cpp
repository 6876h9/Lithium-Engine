#include "world/sprite_actor.hpp"
#include "renderer/geometry.hpp"

SpriteActor::SpriteActor(const std::string& name) : Actor(name) {
    mesh_component = create_component<StaticMeshComponent>("SpriteMesh");
    set_root_component(mesh_component);
    
    shape_type = "Sprite";
    is_invisible = false;

    // Generate square geometry for the sprite
    std::vector<Vertex> verts;
    std::vector<unsigned int> indices;
    generate_square(verts, indices, actor_color);
    mesh_component->set_geometry(verts, indices);
}

void SpriteActor::begin_play() {
    Actor::begin_play();
}

void SpriteActor::tick(float delta_time) {
    Actor::tick(delta_time);
}

void SpriteActor::set_texture(const std::string& texture_path) {
    current_texture = texture_path;
}
