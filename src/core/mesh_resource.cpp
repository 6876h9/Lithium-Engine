#include "core/mesh_resource.hpp"
#include <algorithm>
#include "core/model_importer.hpp"
#include "renderer/gl_loader.hpp"
#include <iostream>

MeshResource::MeshResource(const std::string& filepath) : Resource(filepath) {}

MeshResource::~MeshResource() {
    if (vao != 0) {
        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(1, &vbo);
        glDeleteBuffers(1, &ebo);
    }
    if (skin_vbo != 0) {
        glDeleteBuffers(1, &skin_vbo);
    }
}

bool MeshResource::load_from_disk() {
    if (!ModelImporter::load_mesh_file(filepath, cpu_vertices, cpu_indices, texture_path,
                                       &skeleton, &animation_clips, &cpu_bone_data)) {
        std::cerr << "[ResourceManager] Failed to load mesh from disk: " << filepath << std::endl;
        return false;
    }

    // A skeleton without a matching influence per vertex would skin every vertex to
    // bone 0 with weight 0 and collapse the mesh, so treat a mismatch as "static"
    // rather than drawing something broken.
    if (!cpu_bone_data.empty() && cpu_bone_data.size() != cpu_vertices.size()) {
        std::cerr << "[MeshResource] '" << filepath << "' has " << cpu_bone_data.size()
                  << " skin entries for " << cpu_vertices.size()
                  << " vertices; ignoring its skeleton." << std::endl;
        cpu_bone_data.clear();
        skeleton.bones.clear();
        animation_clips.clear();
    }

    // Bounds, computed here rather than lazily: this is the one place the vertex
    // data is guaranteed to be complete and nothing else is looking at it yet.
    if (!cpu_vertices.empty()) {
        bounds_min = bounds_max = cpu_vertices[0].position;
        for (const Vertex& v : cpu_vertices) {
            bounds_min.x = std::min(bounds_min.x, v.position.x);
            bounds_min.y = std::min(bounds_min.y, v.position.y);
            bounds_min.z = std::min(bounds_min.z, v.position.z);
            bounds_max.x = std::max(bounds_max.x, v.position.x);
            bounds_max.y = std::max(bounds_max.y, v.position.y);
            bounds_max.z = std::max(bounds_max.z, v.position.z);
        }
        const Vector3 half = { (bounds_max.x - bounds_min.x) * 0.5f,
                               (bounds_max.y - bounds_min.y) * 0.5f,
                               (bounds_max.z - bounds_min.z) * 0.5f };
        bounds_radius = half.length();
    }
    return true;
}

bool MeshResource::upload_to_gpu() {
    if (cpu_vertices.empty() || cpu_indices.empty()) return false;

    // Same re-entrancy concern as textures: a hot reload uploads over an existing
    // mesh, so the old buffers must be released or each reload leaks them.
    if (vao != 0) { glDeleteVertexArrays(1, &vao); vao = 0; }
    if (vbo != 0) { glDeleteBuffers(1, &vbo); vbo = 0; }
    if (ebo != 0) { glDeleteBuffers(1, &ebo); ebo = 0; }
    if (skin_vbo != 0) { glDeleteBuffers(1, &skin_vbo); skin_vbo = 0; }

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, cpu_vertices.size() * sizeof(Vertex), cpu_vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, cpu_indices.size() * sizeof(unsigned int), cpu_indices.data(), GL_STATIC_DRAW);

    // Position
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));
    // Color
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color));
    // Normal
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));
    // UV
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));

    // Skinning stream, from its own buffer. Attributes 4 and 5 are left disabled for
    // a static mesh; the vertex shader reads them only when uSkinned is set, and a
    // disabled attribute still supplies a well-defined constant, so the static path
    // needs no separate program.
    if (!cpu_bone_data.empty()) {
        glGenBuffers(1, &skin_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, skin_vbo);
        glBufferData(GL_ARRAY_BUFFER, cpu_bone_data.size() * sizeof(VertexBoneData), cpu_bone_data.data(), GL_STATIC_DRAW);

        // Bone IDs are integers and must go through glVertexAttribIPointer -
        // the float variant would normalise/convert them and the shader would
        // index the palette with garbage.
        glEnableVertexAttribArray(4);
        glVertexAttribIPointer(4, VertexBoneData::kMaxInfluences, GL_INT, sizeof(VertexBoneData),
                               (void*)offsetof(VertexBoneData, bone_ids));

        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, VertexBoneData::kMaxInfluences, GL_FLOAT, GL_FALSE, sizeof(VertexBoneData),
                              (void*)offsetof(VertexBoneData, weights));
    }

    glBindVertexArray(0);

    indices_count = cpu_indices.size();

    // Generate clusters (128 triangles = 384 indices per cluster)
    clusters.clear();
    const size_t INDICES_PER_CLUSTER = 384;
    for (size_t i = 0; i < cpu_indices.size(); i += INDICES_PER_CLUSTER) {
        MeshCluster cluster;
        cluster.index_offset = static_cast<unsigned int>(i);
        cluster.index_count = static_cast<unsigned int>(std::min(INDICES_PER_CLUSTER, cpu_indices.size() - i));
        
        // Calculate bounding sphere
        Vector3 min_bounds = cpu_vertices[cpu_indices[i]].position;
        Vector3 max_bounds = cpu_vertices[cpu_indices[i]].position;
        for (size_t j = 0; j < cluster.index_count; ++j) {
            Vector3 pos = cpu_vertices[cpu_indices[i + j]].position;
            min_bounds.x = std::min(min_bounds.x, pos.x);
            min_bounds.y = std::min(min_bounds.y, pos.y);
            min_bounds.z = std::min(min_bounds.z, pos.z);
            max_bounds.x = std::max(max_bounds.x, pos.x);
            max_bounds.y = std::max(max_bounds.y, pos.y);
            max_bounds.z = std::max(max_bounds.z, pos.z);
        }
        
        cluster.bounds_center = Vector3(
            (min_bounds.x + max_bounds.x) * 0.5f,
            (min_bounds.y + max_bounds.y) * 0.5f,
            (min_bounds.z + max_bounds.z) * 0.5f
        );
        
        cluster.bounds_radius = 0.0f;
        for (size_t j = 0; j < cluster.index_count; ++j) {
            Vector3 pos = cpu_vertices[cpu_indices[i + j]].position;
            float dist_sq = (pos.x - cluster.bounds_center.x) * (pos.x - cluster.bounds_center.x) +
                            (pos.y - cluster.bounds_center.y) * (pos.y - cluster.bounds_center.y) +
                            (pos.z - cluster.bounds_center.z) * (pos.z - cluster.bounds_center.z);
            cluster.bounds_radius = std::max(cluster.bounds_radius, dist_sq);
        }
        cluster.bounds_radius = std::sqrt(cluster.bounds_radius);
        
        clusters.push_back(cluster);
    }

    // Keep CPU memory since offline renderer needs it for ray tracing

    return true;
}
