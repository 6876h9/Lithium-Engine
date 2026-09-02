#include "world/static_mesh_component.hpp"
#include <system_error>
#include <cmath>
#include <filesystem>
#include "physics/physics_engine.hpp"
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

// Builds a tangent frame per vertex from the UV parameterisation.
//
// The tangent is the direction in 3D that texture U increases along, which is what
// lets a normal map - authored in tangent space, so it can be reused on any surface
// - be rotated into world space. It is solved per triangle from the 2x2 UV matrix
// and accumulated at the shared vertices, so a smooth surface gets a smooth frame
// rather than a visible facet per triangle.
void StaticMeshComponent::generate_tangents() {
    tangents.assign(vertices.size(), VertexTangent{});
    if (vertices.empty() || indices.size() < 3) return;

    std::vector<Vector3> tan_u(vertices.size(), Vector3{ 0.0f, 0.0f, 0.0f });
    std::vector<Vector3> tan_v(vertices.size(), Vector3{ 0.0f, 0.0f, 0.0f });

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const unsigned int i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

        const Vector3& p0 = vertices[i0].position;
        const Vector3& p1 = vertices[i1].position;
        const Vector3& p2 = vertices[i2].position;
        const Vector2& w0 = vertices[i0].uv;
        const Vector2& w1 = vertices[i1].uv;
        const Vector2& w2 = vertices[i2].uv;

        const Vector3 e1 = p1 - p0;
        const Vector3 e2 = p2 - p0;
        const float s1 = w1.x - w0.x, t1 = w1.y - w0.y;
        const float s2 = w2.x - w0.x, t2 = w2.y - w0.y;

        // A degenerate UV triangle - two vertices on the same texel, which happens
        // on unwrapped seams and on untextured geometry - has no defined tangent.
        // Skipping it leaves the vertex to its neighbours rather than poisoning the
        // accumulation with an infinity.
        const float determinant = s1 * t2 - s2 * t1;
        if (std::fabs(determinant) < 1e-12f) continue;
        const float r = 1.0f / determinant;

        const Vector3 u_dir{ (t2 * e1.x - t1 * e2.x) * r,
                             (t2 * e1.y - t1 * e2.y) * r,
                             (t2 * e1.z - t1 * e2.z) * r };
        const Vector3 v_dir{ (s1 * e2.x - s2 * e1.x) * r,
                             (s1 * e2.y - s2 * e1.y) * r,
                             (s1 * e2.z - s2 * e1.z) * r };

        tan_u[i0] += u_dir; tan_u[i1] += u_dir; tan_u[i2] += u_dir;
        tan_v[i0] += v_dir; tan_v[i1] += v_dir; tan_v[i2] += v_dir;
    }

    for (size_t i = 0; i < vertices.size(); ++i) {
        const Vector3 n = vertices[i].normal.normalized();
        const Vector3& t = tan_u[i];

        // Gram-Schmidt: remove the part of the tangent that runs along the normal,
        // so the frame is orthogonal even where the accumulated tangents of adjacent
        // faces do not lie in the vertex's own tangent plane.
        Vector3 ortho = t - n * Vector3::dot(n, t);
        const float length = std::sqrt(ortho.x * ortho.x + ortho.y * ortho.y + ortho.z * ortho.z);
        if (length > 1e-8f) {
            ortho = ortho / length;
        } else {
            // No usable tangent: pick any axis perpendicular to the normal. The
            // surface has no UV gradient here, so the choice cannot be wrong -
            // only a NaN would be.
            const Vector3 axis = std::fabs(n.y) < 0.99f ? Vector3{ 0.0f, 1.0f, 0.0f }
                                                        : Vector3{ 1.0f, 0.0f, 0.0f };
            ortho = Vector3::cross(axis, n).normalized();
        }

        // Handedness. Mirrored UVs - how a symmetric character is textured from half
        // a map - flip the bitangent, and without this the normal map lights the
        // mirrored half backwards.
        const float handedness =
            (Vector3::dot(Vector3::cross(n, ortho), tan_v[i]) < 0.0f) ? -1.0f : 1.0f;
        tangents[i].tangent = { ortho.x, ortho.y, ortho.z, handedness };
    }
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

            // Normal maps are found by convention rather than declared: almost no
            // format the importer reads carries one, but almost every art pipeline
            // names it after the albedo it belongs to. The suffixes below are the
            // ones actually in use; the first that exists on disk wins, and a mesh
            // with none simply renders with its geometric normal as before.
            static const char* kSuffixes[] = { "_normal", "_Normal", "_nrm", "_n", "_NRM" };
            const size_t dot = texture_path.find_last_of('.');
            if (dot != std::string::npos) {
                const std::string stem = texture_path.substr(0, dot);
                const std::string extension = texture_path.substr(dot);
                for (const char* suffix : kSuffixes) {
                    const std::string candidate = stem + suffix + extension;
                    std::error_code ec;
                    if (std::filesystem::exists(candidate, ec)) {
                        normal_texture = ResourceManager::get().load_async<TextureResource>(candidate);
                        break;
                    }
                }
            }
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

            // Foot placement, wired up but left switched off. Rigs name their bones
            // by no standard at all, so the common conventions are tried in turn and
            // whichever resolves wins; a rig matching none of them simply gets no
            // chains, and the editor says so.
            static const char* kPelvisNames[] = { "pelvis", "Pelvis", "hips", "Hips",
                                                  "mixamorig:Hips", "Bip01_Pelvis" };
            static const char* kLeftFootNames[] = { "foot_l", "LeftFoot", "left_foot",
                                                    "mixamorig:LeftFoot", "Bip01_L_Foot" };
            static const char* kRightFootNames[] = { "foot_r", "RightFoot", "right_foot",
                                                     "mixamorig:RightFoot", "Bip01_R_Foot" };
            const Skeleton& skeleton = mesh_resource->get_skeleton();
            const auto first_present = [&skeleton](const char* const* candidates, size_t count) {
                for (size_t i = 0; i < count; ++i) {
                    if (skeleton.find_bone(candidates[i]) >= 0) return std::string(candidates[i]);
                }
                return std::string();
            };

            const std::string pelvis = first_present(kPelvisNames, std::size(kPelvisNames));
            const std::string left = first_present(kLeftFootNames, std::size(kLeftFootNames));
            const std::string right = first_present(kRightFootNames, std::size(kRightFootNames));
            if (!pelvis.empty() && (!left.empty() || !right.empty())) {
                std::vector<std::string> feet;
                if (!left.empty()) feet.push_back(left);
                if (!right.empty()) feet.push_back(right);
                animator->configure_foot_placement(pelvis, feet);

                // The probe is a callback so this component needs no compile-time
                // dependency on the physics engine's query types, and so a test can
                // stand the solver on a flat plane with no physics world at all.
                animator->set_ground_probe([](const DVector3& from, const Vector3& direction,
                                              float max_distance, Vector3& out_point,
                                              Vector3& out_normal) {
                    RaycastHit hit;
                    if (!PhysicsEngine::get_instance().raycast(from, direction, max_distance, hit)) {
                        return false;
                    }
                    out_point = hit.point;
                    out_normal = hit.normal;
                    return true;
                });
            }
        }
    }

    if (animator) {
        // The solver traces against the physics world in world space, so it needs
        // to know where this mesh currently is. Set every frame: an actor that
        // moves between frames would otherwise trace under where it used to be.
        // Absolute world matrix: the trace happens against the physics world, which
        // is in absolute coordinates, not the camera-relative space rendering uses.
        animator->set_world_transform(transform.get_matrix());
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

    // Tangent frame (layout = 6), from its own buffer. 4 and 5 are the skinning
    // streams; 6 is the first slot free after them.
    if (tangents.size() != vertices.size()) generate_tangents();
    if (!tangents.empty()) {
        if (tangent_vbo == 0) glGenBuffers(1, &tangent_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, tangent_vbo);
        glBufferData(GL_ARRAY_BUFFER, tangents.size() * sizeof(VertexTangent),
                     tangents.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(6);
        glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(VertexTangent), (void*)0);
    }

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
