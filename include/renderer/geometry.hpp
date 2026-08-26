#pragma once

#include "world/static_mesh_component.hpp"
#include <vector>
#include <cmath>

inline void generate_cube(std::vector<Vertex>& out_vertices, std::vector<unsigned int>& out_indices, const Vector3& color = {1.0f, 1.0f, 1.0f}) {
    out_vertices = {
        // Front face (vertices with position-normalized normals)
        {{-0.5f, -0.5f,  0.5f}, color, Vector3{-0.5f, -0.5f,  0.5f}.normalized()}, // 0
        {{ 0.5f, -0.5f,  0.5f}, color, Vector3{ 0.5f, -0.5f,  0.5f}.normalized()}, // 1
        {{ 0.5f,  0.5f,  0.5f}, color, Vector3{ 0.5f,  0.5f,  0.5f}.normalized()}, // 2
        {{-0.5f,  0.5f,  0.5f}, color, Vector3{-0.5f,  0.5f,  0.5f}.normalized()}, // 3
        // Back face
        {{-0.5f, -0.5f, -0.5f}, color, Vector3{-0.5f, -0.5f, -0.5f}.normalized()}, // 4
        {{ 0.5f, -0.5f, -0.5f}, color, Vector3{ 0.5f, -0.5f, -0.5f}.normalized()}, // 5
        {{ 0.5f,  0.5f, -0.5f}, color, Vector3{ 0.5f,  0.5f, -0.5f}.normalized()}, // 6
        {{-0.5f,  0.5f, -0.5f}, color, Vector3{-0.5f,  0.5f, -0.5f}.normalized()}  // 7
    };

    out_indices = {
        0, 1, 2, 2, 3, 0, // Front
        1, 5, 6, 6, 2, 1, // Right
        5, 4, 7, 7, 6, 5, // Back
        4, 0, 3, 3, 7, 4, // Left
        3, 2, 6, 6, 7, 3, // Top
        4, 5, 1, 1, 0, 4  // Bottom
    };
}

inline void generate_square(std::vector<Vertex>& out_vertices, std::vector<unsigned int>& out_indices, const Vector3& color = {1.0f, 1.0f, 1.0f}) {
    Vector3 flat_normal = {0.0f, 1.0f, 0.0f};
    out_vertices = {
        {{-0.5f,  0.0f,  0.5f}, color, flat_normal},
        {{ 0.5f,  0.0f,  0.5f}, color, flat_normal},
        {{ 0.5f,  0.0f, -0.5f}, color, flat_normal},
        {{-0.5f,  0.0f, -0.5f}, color, flat_normal}
    };

    out_indices = {
        0, 1, 2,
        2, 3, 0
    };
}

inline void generate_sphere(std::vector<Vertex>& out_vertices, std::vector<unsigned int>& out_indices, float rx = 0.5f, float ry = 0.5f, float rz = 0.5f, const Vector3& color = {1.0f, 1.0f, 1.0f}) {
    const unsigned int X_SEGMENTS = 24;
    const unsigned int Y_SEGMENTS = 24;
    const float PI = 3.1415926535f;

    for (unsigned int x = 0; x <= X_SEGMENTS; ++x) {
        for (unsigned int y = 0; y <= Y_SEGMENTS; ++y) {
            float xSegment = (float)x / (float)X_SEGMENTS;
            float ySegment = (float)y / (float)Y_SEGMENTS;
            float xPos = rx * std::cos(xSegment * 2.0f * PI) * std::sin(ySegment * PI);
            float yPos = ry * std::cos(ySegment * PI);
            float zPos = rz * std::sin(xSegment * 2.0f * PI) * std::sin(ySegment * PI);

            Vector3 normal = Vector3{xPos, yPos, zPos}.normalized();

            out_vertices.push_back({{xPos, yPos, zPos}, color, normal});
        }
    }

    for (unsigned int y = 0; y < Y_SEGMENTS; ++y) {
        for (unsigned int x = 0; x < X_SEGMENTS; ++x) {
            out_indices.push_back((y + 1) * (X_SEGMENTS + 1) + x);
            out_indices.push_back(y * (X_SEGMENTS + 1) + x);
            out_indices.push_back(y * (X_SEGMENTS + 1) + x + 1);

            out_indices.push_back((y + 1) * (X_SEGMENTS + 1) + x);
            out_indices.push_back(y * (X_SEGMENTS + 1) + x + 1);
            out_indices.push_back((y + 1) * (X_SEGMENTS + 1) + x + 1);
        }
    }
}
