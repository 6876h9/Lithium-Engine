#pragma once

#include "world/actor.hpp"
#include "world/static_mesh_component.hpp"
#include "renderer/geometry.hpp"
#include <string>

class EditorPrimitiveActor : public Actor {
public:
    EditorPrimitiveActor(const std::string& name, const std::string& type)
        : Actor(name) {
        shape_type = type;

        // Create and register the mesh component immediately in the constructor
        mesh = create_component<StaticMeshComponent>("PrimitiveMesh");
        set_root_component(mesh);

        // Generate geometry based on the type
        std::vector<Vertex> verts;
        std::vector<unsigned int> indices;

        if (shape_type == "Cube") {
            generate_cube(verts, indices, actor_color);
        } else if (shape_type == "Square" || shape_type == "Plane") {
            generate_square(verts, indices, actor_color);
        } else if (shape_type == "Sphere") {
            generate_sphere(verts, indices, 0.5f, 0.5f, 0.5f, actor_color);
        } else if (shape_type == "Oval") {
            // Spheroid with different radius (oval shape)
            generate_sphere(verts, indices, 0.4f, 0.7f, 0.4f, actor_color);
        } else {
            // Default to cube fallback
            generate_cube(verts, indices, actor_color);
        }

        mesh->set_geometry(verts, indices);
    }

    virtual void begin_play() override {
        Actor::begin_play();
        // Geometry is already generated and bound
    }

private:
    StaticMeshComponent* mesh = nullptr;
};
