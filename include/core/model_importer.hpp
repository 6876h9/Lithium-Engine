#pragma once

#include <string>
#include <vector>
#include "world/static_mesh_component.hpp"
#include "world/skeleton.hpp"
#include "world/animation_clip.hpp"

class ModelImporter {
public:
    // Imports a 3D model (FBX, glTF/GLB, OBJ, DAE, etc. - anything Assimp supports).
    // Walks the full node hierarchy, merges every mesh in the scene into one
    // combined vertex/index buffer (baking each node's world transform into
    // the vertex positions/normals), and resolves the first diffuse/base-color
    // texture it finds (embedded or external, copied into Content/Textures/).
    // Saves the result into the Content/ directory as a custom .mesh file.
    // Returns the path to the newly created .mesh file, or an empty string on failure.
    static std::string import_model(const std::string& filepath);

    // Writes a static .mesh file: the same layout import_model produces, minus the
    // skin block. Exposed so generated geometry - LOD reductions - can be saved as
    // an ordinary asset instead of needing a second file format.
    static bool write_static_mesh_file(const std::string& filepath,
                                       const std::vector<Vertex>& vertices,
                                       const std::vector<unsigned int>& indices,
                                       const std::string& texture_path);

    // Loads a .mesh file from disk into memory (vertices, indices, and the
    // resolved diffuse texture path, if any).
    //
    // The last three parameters receive the skinning data, and are optional: pass
    // null to load a mesh as pure geometry. They are also left untouched for a
    // .mesh file written before skinning existed - such a file simply ends after
    // the texture path, and that is a valid static mesh, not a read error.
    static bool load_mesh_file(const std::string& filepath, std::vector<Vertex>& out_vertices,
                                std::vector<unsigned int>& out_indices, std::string& out_texture_path,
                                Skeleton* out_skeleton = nullptr,
                                std::vector<AnimationClip>* out_clips = nullptr,
                                std::vector<VertexBoneData>* out_bone_data = nullptr);
};
