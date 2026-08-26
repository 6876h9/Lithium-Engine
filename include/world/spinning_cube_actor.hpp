#pragma once

#include "world/actor.hpp"
#include "world/static_mesh_component.hpp"
#include <vector>

class SpinningCubeActor : public Actor {
public:
    SpinningCubeActor(const std::string& name) : Actor(name) {}

    virtual void begin_play() override {
        Actor::begin_play();

        // Create Static Mesh Component
        mesh = create_component<StaticMeshComponent>("CubeMesh");
        set_root_component(mesh);

        // Cube Vertices
        std::vector<Vertex> vertices = {
            // Front face
            {{-0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}}, // 0
            {{ 0.5f, -0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}}, // 1
            {{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}}, // 2
            {{-0.5f,  0.5f,  0.5f}, {1.0f, 1.0f, 0.0f}}, // 3
            // Back face
            {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 1.0f}}, // 4
            {{ 0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 1.0f}}, // 5
            {{ 0.5f,  0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}}, // 6
            {{-0.5f,  0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}}  // 7
        };

        // Cube Indices
        std::vector<unsigned int> indices = {
            // Front
            0, 1, 2, 2, 3, 0,
            // Right
            1, 5, 6, 6, 2, 1,
            // Back
            5, 4, 7, 7, 6, 5,
            // Left
            4, 0, 3, 3, 7, 4,
            // Top
            3, 2, 6, 6, 7, 3,
            // Bottom
            4, 5, 1, 1, 0, 4
        };

        mesh->set_geometry(vertices, indices);

        // Set initial scale and position
        mesh->transform.scale = {1.0f, 1.0f, 1.0f};
        mesh->transform.position = {0.0f, 0.0f, 0.0f};
    }

    virtual void tick(float delta_time) override {
        Actor::tick(delta_time);

        // Spin the cube over time
        if (mesh) {
            mesh->transform.rotation.x += rotation_speed.x * delta_time;
            mesh->transform.rotation.y += rotation_speed.y * delta_time;
        }
    }

    void set_rotation_speed(const Vector3& speed) { rotation_speed = speed; }

private:
    StaticMeshComponent* mesh = nullptr;
    Vector3 rotation_speed = {0.8f, 1.2f, 0.0f};
};
