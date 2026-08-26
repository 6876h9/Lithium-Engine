#include "world/static_mesh_component.hpp"
#include <algorithm>
#include "core/mesh_resource.hpp"
#include "core/texture_resource.hpp"
#include "core/resource_manager.hpp"
#include "world/animation_player.hpp"
#include "renderer/gl_loader.hpp"
#include <iostream>

StaticMeshComponent::StaticMeshComponent(Actor* owner, const std::string& name)
    : SceneComponent(owner, name) {}

StaticMeshComponent::~StaticMeshComponent() {
    if (lightmap_vao) glDeleteVertexArrays(1, &lightmap_vao);
    if (lightmap_uv_vbo) glDeleteBuffers(1, &lightmap_uv_vbo);

    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (ebo) glDeleteBuffers(1, &ebo);
}

void StaticMeshComponent::resolve_diffuse_texture() const {
    // Once the backing mesh has finished loading, kick off an async load of
    // whatever diffuse texture the importer resolved for it (if any). This is
    // called from render() rather than tick() because actor ticking is gated
    // to PlayInEditor mode, while rendering (and therefore the need for the
    // texture) happens in plain Editor mode too.
    if (!texture_resolve_attempted && mesh_resource && mesh_resource->get_state() == ResourceState::LoadedGPU) {
        texture_resolve_attempted = true;
        const std::string& texture_path = mesh_resource->get_texture_path();
        if (!texture_path.empty() && !diffuse_texture) {
            diffuse_texture = ResourceManager::get().load_async<TextureResource>(texture_path);
        }
    }
}

void StaticMeshComponent::set_mesh_resource(std::shared_ptr<MeshResource> resource) {
    mesh_resource = resource;
    animator.reset();
    animator_resolved = false;
}

void StaticMeshComponent::update_animation(float delta_time) {
    if (!animator_resolved) {
        // Deferred until the resource is actually resident: meshes load on a worker
        // thread, so at set_mesh_resource() time there is no skeleton to inspect yet.
        if (!mesh_resource) return;
        ResourceState state = mesh_resource->get_state();
        if (state == ResourceState::Failed) {
            animator_resolved = true;
            return;
        }
        if (state != ResourceState::LoadedGPU) return;

        animator_resolved = true;
        if (mesh_resource->is_skinned()) {
            animator = std::make_unique<AnimationPlayer>(&mesh_resource->get_skeleton(),
                                                          &mesh_resource->get_animation_clips());
            // Start on the first clip so an imported character moves as soon as it is
            // dropped into the scene, rather than standing in bind pose until someone
            // finds the animation panel.
            if (animator->get_clip_count() > 0) {
                animator->play(0, true);
            }
        }
    }

    if (animator) {
        animator->update(delta_time);
    }
}

void StaticMeshComponent::set_geometry(const std::vector<Vertex>& new_vertices, const std::vector<unsigned int>& new_indices) {
    vertices = new_vertices;
    indices = new_indices;

    has_local_bounds = !vertices.empty();
    if (has_local_bounds) {
        local_bounds_min = local_bounds_max = vertices[0].position;
        for (const Vertex& v : vertices) {
            local_bounds_min.x = std::min(local_bounds_min.x, v.position.x);
            local_bounds_min.y = std::min(local_bounds_min.y, v.position.y);
            local_bounds_min.z = std::min(local_bounds_min.z, v.position.z);
            local_bounds_max.x = std::max(local_bounds_max.x, v.position.x);
            local_bounds_max.y = std::max(local_bounds_max.y, v.position.y);
            local_bounds_max.z = std::max(local_bounds_max.z, v.position.z);
        }
    }

    update_gpu_buffers();
}

void StaticMeshComponent::set_lightmap_uvs(const std::vector<Vector2>& uvs) {
    lightmap_uvs = uvs;
    lightmap_dirty = true;
}

unsigned int StaticMeshComponent::ensure_lightmap_vao(unsigned int source_vbo,
                                                      unsigned int source_ebo) const {
    if (lightmap_uvs.empty() || source_vbo == 0) return 0;

    if (lightmap_vao == 0) {
        glGenVertexArrays(1, &lightmap_vao);
        glGenBuffers(1, &lightmap_uv_vbo);
        lightmap_dirty = true;
    }

    glBindVertexArray(lightmap_vao);

    if (lightmap_dirty) {
        glBindBuffer(GL_ARRAY_BUFFER, lightmap_uv_vbo);
        glBufferData(GL_ARRAY_BUFFER, lightmap_uvs.size() * sizeof(Vector2),
                     lightmap_uvs.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(10);
        glVertexAttribPointer(10, 2, GL_FLOAT, GL_FALSE, sizeof(Vector2), (void*)0);
        lightmap_dirty = false;
    }

    // The geometry itself is re-pointed every time: the mesh resource can finish
    // streaming, or be swapped, after this array was first built.
    glBindBuffer(GL_ARRAY_BUFFER, source_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
    if (source_ebo != 0) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, source_ebo);

    glBindVertexArray(0);
    return lightmap_vao;
}

bool StaticMeshComponent::get_local_bounds(Vector3& out_min, Vector3& out_max) const {
    if (mesh_resource) {
        // A resource that has not finished loading has no vertices to bound, and
        // reporting a zero-sized box would make every culling test say "invisible".
        ResourceState state = mesh_resource->get_state();
        if (state != ResourceState::LoadedCPU && state != ResourceState::LoadedGPU) return false;
        out_min = mesh_resource->get_bounds_min();
        out_max = mesh_resource->get_bounds_max();
        return true;
    }
    if (!has_local_bounds) return false;
    out_min = local_bounds_min;
    out_max = local_bounds_max;
    return true;
}

void StaticMeshComponent::update_gpu_buffers() {
    if (!vao) {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);
    }

    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

    // Position attribute (layout = 0)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));

    // Color attribute (layout = 1)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color));

    // Normal attribute (layout = 2)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));

    // UV attribute (layout = 3)
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));

    glBindVertexArray(0);
    
    indices_count = indices.size();

    // Generate clusters (128 triangles = 384 indices per cluster)
    clusters.clear();
    const size_t INDICES_PER_CLUSTER = 384;
    for (size_t i = 0; i < indices.size(); i += INDICES_PER_CLUSTER) {
        MeshCluster cluster;
        cluster.index_offset = static_cast<unsigned int>(i);
        cluster.index_count = static_cast<unsigned int>(std::min(INDICES_PER_CLUSTER, indices.size() - i));
        
        // Calculate bounding sphere
        Vector3 min_bounds = vertices[indices[i]].position;
        Vector3 max_bounds = vertices[indices[i]].position;
        for (size_t j = 0; j < cluster.index_count; ++j) {
            Vector3 pos = vertices[indices[i + j]].position;
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
            Vector3 pos = vertices[indices[i + j]].position;
            float dist_sq = (pos.x - cluster.bounds_center.x) * (pos.x - cluster.bounds_center.x) +
                            (pos.y - cluster.bounds_center.y) * (pos.y - cluster.bounds_center.y) +
                            (pos.z - cluster.bounds_center.z) * (pos.z - cluster.bounds_center.z);
            cluster.bounds_radius = std::max(cluster.bounds_radius, dist_sq);
        }
        cluster.bounds_radius = std::sqrt(cluster.bounds_radius);
        
        clusters.push_back(cluster);
    }
}

void StaticMeshComponent::render(const MeshResource* override_resource) const {
    resolve_diffuse_texture();
    // An LOD level that has not finished streaming falls through to this
    // component's own mesh rather than drawing nothing: a hole in the world while
    // an asset loads is far worse than a frame at the wrong detail level.
    if (override_resource && override_resource->get_state() == ResourceState::LoadedGPU) {
        glBindVertexArray(override_resource->get_vao());
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(override_resource->get_indices_count()), GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
        return;
    }
    if (mesh_resource && mesh_resource->get_state() == ResourceState::LoadedGPU) {
        // A baked actor draws through its own vertex array, which carries the second
        // uv set alongside the resource's shared geometry.
        unsigned int array = ensure_lightmap_vao(mesh_resource->get_vbo(), mesh_resource->get_ebo());
        if (array == 0) array = mesh_resource->get_vao();
        glBindVertexArray(array);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh_resource->get_indices_count()), GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    } else if (!mesh_resource && vao && indices_count > 0) {
        unsigned int array = ensure_lightmap_vao(vbo, ebo);
        if (array == 0) array = vao;
        glBindVertexArray(array);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indices_count), GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }
}

void StaticMeshComponent::render_lithite(const std::vector<MeshCluster>& visible_clusters) const {
    if (visible_clusters.empty()) return;
    resolve_diffuse_texture();

    std::vector<GLsizei> counts;
    std::vector<const void*> indices;
    counts.reserve(visible_clusters.size());
    indices.reserve(visible_clusters.size());
    
    for (const auto& cluster : visible_clusters) {
        counts.push_back(cluster.index_count);
        indices.push_back((const void*)(cluster.index_offset * sizeof(unsigned int)));
    }
    
    if (mesh_resource && mesh_resource->get_state() == ResourceState::LoadedGPU) {
        glBindVertexArray(mesh_resource->get_vao());
        glMultiDrawElements(GL_TRIANGLES, counts.data(), GL_UNSIGNED_INT, indices.data(), static_cast<GLsizei>(visible_clusters.size()));
        glBindVertexArray(0);
    } else if (!mesh_resource && vao && indices_count > 0) {
        glBindVertexArray(vao);
        glMultiDrawElements(GL_TRIANGLES, counts.data(), GL_UNSIGNED_INT, indices.data(), static_cast<GLsizei>(visible_clusters.size()));
        glBindVertexArray(0);
    }
}
