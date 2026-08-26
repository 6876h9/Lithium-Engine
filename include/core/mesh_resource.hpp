#pragma once

#include "core/resource.hpp"
#include "world/static_mesh_component.hpp" // For Vertex
#include "world/skeleton.hpp"
#include "world/animation_clip.hpp"
#include <vector>

class MeshResource : public Resource {
public:
    MeshResource(const std::string& filepath);
    virtual ~MeshResource();

    virtual bool load_from_disk() override;
    virtual bool upload_to_gpu() override;

    unsigned int get_vao() const { return vao; }
    // The raw buffers behind that VAO. Needed by instanced drawing, which has to
    // build its own vertex array combining this geometry with a per-instance stream.
    unsigned int get_vbo() const { return vbo; }
    unsigned int get_ebo() const { return ebo; }
    unsigned int get_indices_count() const { return indices_count; }

    // Local-space bounding box, computed once when the vertices are read. Derived
    // data, so it is not part of the .mesh format on disk - LOD selection and
    // occlusion culling both need it, and recomputing it per frame would mean
    // walking every vertex of every mesh in the scene.
    const Vector3& get_bounds_min() const { return bounds_min; }
    const Vector3& get_bounds_max() const { return bounds_max; }
    Vector3 get_bounds_center() const {
        return { (bounds_min.x + bounds_max.x) * 0.5f,
                 (bounds_min.y + bounds_max.y) * 0.5f,
                 (bounds_min.z + bounds_max.z) * 0.5f };
    }
    // Radius of the sphere around that box, in local units.
    float get_bounds_radius() const { return bounds_radius; }

    const std::vector<Vertex>& get_cpu_vertices() const { return cpu_vertices; }
    const std::vector<unsigned int>& get_cpu_indices() const { return cpu_indices; }
    const std::vector<MeshCluster>& get_clusters() const { return clusters; }

    // Path to the diffuse/base-color texture resolved at import time, relative
    // to the working directory (e.g. "Content/Textures/foo.png"). Empty if the
    // source model had no resolvable material texture.
    const std::string& get_texture_path() const { return texture_path; }

    // Skinning data, present only for models that were imported with a skeleton.
    // It lives on the resource rather than the component because it is per-asset:
    // every actor sharing this mesh shares one skeleton and one set of clips, and
    // only the playback cursor differs between them.
    bool is_skinned() const { return !skeleton.empty() && !cpu_bone_data.empty(); }
    const Skeleton& get_skeleton() const { return skeleton; }
    const std::vector<AnimationClip>& get_animation_clips() const { return animation_clips; }

private:
    Vector3 bounds_min = { 0.0f, 0.0f, 0.0f };
    Vector3 bounds_max = { 0.0f, 0.0f, 0.0f };
    float bounds_radius = 0.0f;

    std::vector<Vertex> cpu_vertices;
    std::vector<unsigned int> cpu_indices;
    std::vector<MeshCluster> clusters;
    std::string texture_path;

    Skeleton skeleton;
    std::vector<AnimationClip> animation_clips;
    // Parallel to cpu_vertices; empty for a static mesh.
    std::vector<VertexBoneData> cpu_bone_data;

    unsigned int vbo = 0;
    unsigned int vao = 0;
    unsigned int ebo = 0;
    // Second vertex buffer holding cpu_bone_data. Kept separate from vbo so the
    // static-mesh upload path is untouched and the interleaved Vertex stride stays
    // exactly what the .mesh format on disk describes.
    unsigned int skin_vbo = 0;
    unsigned int indices_count = 0;
};
