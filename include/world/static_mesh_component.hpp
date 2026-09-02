#pragma once

#include "world/component.hpp"
#include <vector>
#include <memory>
#include <string>

struct Vertex {
    Vector3 position;
    Vector3 color;
    Vector3 normal;
    Vector2 uv;
};

// Per-vertex skinning influences, held in a stream parallel to Vertex rather than
// as extra Vertex fields. Vertex is written raw into .mesh files and read back
// verbatim by the path tracer, so widening it would silently misparse every asset
// already on disk; a second stream also costs nothing for the static meshes that
// make up most of a scene.
struct VertexBoneData {
    static constexpr int kMaxInfluences = 4;
    // Index into Skeleton::bones. Slots past the vertex's influence count carry
    // weight 0, so they can safely point at bone 0.
    int   bone_ids[kMaxInfluences] = { 0, 0, 0, 0 };
    float weights[kMaxInfluences]  = { 0.0f, 0.0f, 0.0f, 0.0f };
};

// Per-vertex tangent frame, in a stream parallel to Vertex for the same reason
// VertexBoneData is: Vertex is written raw into .mesh files and read back verbatim
// by the path tracer, so adding a field to it would silently misparse every asset
// already on disk.
//
// xyz is the tangent; w is +1 or -1 and records whether the UV winding is mirrored.
// The bitangent is cross(normal, tangent) * w, so one float carries what would
// otherwise be a second three-component stream - and mirrored UVs are extremely
// common, because that is how anyone gets a symmetric character out of half a texture.
struct VertexTangent {
    Vector4 tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
};

struct MeshCluster {
    Vector3 bounds_center;
    float bounds_radius;
    unsigned int index_offset;
    unsigned int index_count;
    unsigned int pad1;
    unsigned int pad2;
};

class StaticMeshComponent : public SceneComponent {
public:
    StaticMeshComponent(Actor* owner, const std::string& name);
    ~StaticMeshComponent();

    void set_geometry(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices);
    // Draws this mesh. `override_resource` substitutes a different mesh for the
    // draw without changing what the component owns, which is how an LOD group
    // swaps in a cheaper mesh without destroying this one's animator or textures.
    void render(const class MeshResource* override_resource = nullptr) const;
    void render_lithite(const std::vector<MeshCluster>& visible_clusters) const;
    const std::vector<Vertex>& get_vertices() const { return vertices; }
    const std::vector<unsigned int>& get_indices() const { return indices; }
    const std::vector<MeshCluster>& get_clusters() const { return clusters; }

    // Local-space bounds of whatever this component actually draws - the streamed
    // resource's if it has one, otherwise its own generated geometry. Returns false
    // when there is nothing to bound yet, which a streaming mesh is until it lands.
    bool get_local_bounds(Vector3& out_min, Vector3& out_max) const;

    // --- Baked lighting ------------------------------------------------------
    // Second uv set, one per vertex, addressing this actor's region of the scene
    // lightmap atlas. Per component rather than per mesh asset: two actors sharing a
    // mesh sit in different parts of the atlas because they are lit differently.
    // Empty means this actor was not baked and falls back to the probe grid.
    void set_lightmap_uvs(const std::vector<Vector2>& uvs);
    bool has_lightmap() const { return !lightmap_uvs.empty(); }
    void clear_lightmap() { lightmap_uvs.clear(); lightmap_dirty = true; }

    // Out-of-line because it destroys the AnimationPlayer, which is only forward
    // declared here: a different asset means a different skeleton, so the old
    // player is meaningless (it points into the previous resource's clip list).
    void set_mesh_resource(std::shared_ptr<class MeshResource> resource);
    std::shared_ptr<class MeshResource> get_mesh_resource() const { return mesh_resource; }

    // Advances this mesh's skeletal pose. Called once per frame from the logic
    // thread, in Editor as well as Play mode, so a clip previews in the viewport
    // without having to enter Play - actor tick() is gated to PlayInEditor and so
    // cannot drive this.
    //
    // Also performs the one-time check for whether the backing mesh is skinned at
    // all; that cannot happen at set_mesh_resource() time because the resource
    // loads asynchronously and has no skeleton yet when it is handed over.
    void update_animation(float delta_time);

    // The skeletal player for this mesh, or null if the mesh has no skeleton or
    // has not finished loading. Only valid on the thread holding the scene lock.
    class AnimationPlayer* get_animator() const { return animator.get(); }

    // Diffuse/base-color texture pulled in from the imported model's material (may be null).
    void set_diffuse_texture(std::shared_ptr<class TextureResource> texture) { diffuse_texture = texture; }
    std::shared_ptr<class TextureResource> get_diffuse_texture() const { return diffuse_texture; }

    // Normal map. Held separately from the diffuse texture because it is resolved
    // by convention from the diffuse path rather than by the importer, and because
    // a mesh legitimately has one without the other.
    void set_normal_texture(std::shared_ptr<class TextureResource> texture) { normal_texture = texture; }
    std::shared_ptr<class TextureResource> get_normal_texture() const { return normal_texture; }
    const std::vector<VertexTangent>& get_tangents() const { return tangents; }
    // Builds the tangent frames from positions, UVs and normals. Called when
    // geometry is set; safe to call again after the mesh changes.
    void generate_tangents();

    // Normally the lazy resolve is driven by render(). TESLA has to ask for it
    // itself: in path-tracing mode the rasteriser's mesh path never runs, so an
    // imported mesh's texture would otherwise never be requested at all.
    void ensure_diffuse_texture_requested() const { resolve_diffuse_texture(); }

    unsigned int get_vao() const { return vao; }
    size_t get_indices_count_internal() const { return indices_count; }

private:
    // Held by unique_ptr to an incomplete type; the destructor below is out-of-line
    // in the .cpp, which is what makes that legal.
    std::unique_ptr<class AnimationPlayer> animator;
    bool animator_resolved = false;

    std::shared_ptr<class MeshResource> mesh_resource = nullptr;
    // Lazily resolved from render() (see resolve_diffuse_texture), which runs
    // regardless of Editor/PlayInEditor state - unlike tick(), which is gated
    // to PlayInEditor and so can't be relied on for this.
    mutable std::shared_ptr<class TextureResource> diffuse_texture = nullptr;
    mutable std::shared_ptr<class TextureResource> normal_texture = nullptr;
    mutable bool texture_resolve_attempted = false;

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    size_t indices_count = 0;

    std::vector<MeshCluster> clusters;
    // Bounds of `vertices`, for the generated-geometry path. A resource carries its
    // own, computed when it loads.
    Vector3 local_bounds_min = { 0.0f, 0.0f, 0.0f };
    Vector3 local_bounds_max = { 0.0f, 0.0f, 0.0f };
    bool has_local_bounds = false;

    unsigned int vao = 0;
    unsigned int vbo = 0;
    // Tangent frames, uploaded alongside the vertex buffer into attribute 6.
    unsigned int tangent_vbo = 0;
    std::vector<VertexTangent> tangents;
    unsigned int ebo = 0;

    // Lightmapped draws need their own vertex array: the geometry buffers may belong
    // to a shared MeshResource, but the second uv stream belongs to this component.
    std::vector<Vector2> lightmap_uvs;
    mutable unsigned int lightmap_vao = 0;
    mutable unsigned int lightmap_uv_vbo = 0;
    mutable bool lightmap_dirty = false;
    // Rebuilds the lightmap vertex array against the given geometry buffers.
    // Returns the array to draw with, or 0 when there is no lightmap.
    unsigned int ensure_lightmap_vao(unsigned int source_vbo, unsigned int source_ebo) const;

    void update_gpu_buffers();
    void resolve_diffuse_texture() const;
};
