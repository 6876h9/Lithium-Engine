#include "core/model_importer.hpp"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

namespace {

constexpr uint32_t MESH_FILE_MAGIC = 0x4C4D5348; // "LMSH"
// Marks the optional skinning section appended after the texture path. A .mesh
// written before skinning existed simply ends there, so its absence means "static
// mesh", not "corrupt file" - which is what keeps already-imported assets loading.
constexpr uint32_t SKIN_BLOCK_MAGIC = 0x4C534B4E; // "LSKN"
constexpr uint32_t SKIN_BLOCK_VERSION = 1;

// Above this the palette no longer fits the vertex shader's uniform array; see
// Renderer::kMaxBones, which this must not exceed.
constexpr int kMaxSkeletonBones = 128;

Vector3 to_vector3(const aiVector3D& v) {
    return { v.x, v.y, v.z };
}

// Assimp stores matrices row-major (a1..a4 is the first row); this engine's
// Matrix4x4 is column-major, with element (row, col) at m[col * 4 + row]. The
// conversion is therefore a transpose, not a memcpy - getting this wrong produces
// a skeleton that looks plausible at rest and shears apart as soon as it moves.
Matrix4x4 to_matrix4x4(const aiMatrix4x4& in) {
    Matrix4x4 out;
    out.m[0] = in.a1; out.m[4] = in.a2; out.m[8]  = in.a3; out.m[12] = in.a4;
    out.m[1] = in.b1; out.m[5] = in.b2; out.m[9]  = in.b3; out.m[13] = in.b4;
    out.m[2] = in.c1; out.m[6] = in.c2; out.m[10] = in.c3; out.m[14] = in.c4;
    out.m[3] = in.d1; out.m[7] = in.d2; out.m[11] = in.d3; out.m[15] = in.d4;
    return out;
}

Vector4 to_quaternion(const aiQuaternion& q) {
    return { q.x, q.y, q.z, q.w };
}

// Adds one influence to a vertex, keeping only the four heaviest. Assimp hands
// influences out per-bone rather than per-vertex, so a vertex's full set is only
// known once every bone has been visited; dropping the lightest as we go gives the
// same result as sorting afterwards without holding every influence in memory.
void add_bone_influence(VertexBoneData& data, int bone_id, float weight) {
    if (weight <= 0.0f) return;

    int weakest = -1;
    for (int i = 0; i < VertexBoneData::kMaxInfluences; ++i) {
        if (data.weights[i] == 0.0f) {
            data.bone_ids[i] = bone_id;
            data.weights[i] = weight;
            return;
        }
        if (weakest == -1 || data.weights[i] < data.weights[weakest]) {
            weakest = i;
        }
    }
    if (weakest != -1 && weight > data.weights[weakest]) {
        data.bone_ids[weakest] = bone_id;
        data.weights[weakest] = weight;
    }
}

// Renormalises so the four weights sum to 1. Necessary because dropping the
// lightest influences above removes some of the original total, and because some
// exporters simply do not normalise.
void normalize_bone_weights(VertexBoneData& data) {
    float sum = 0.0f;
    for (int i = 0; i < VertexBoneData::kMaxInfluences; ++i) sum += data.weights[i];
    if (sum <= 0.0f) return;
    for (int i = 0; i < VertexBoneData::kMaxInfluences; ++i) data.weights[i] /= sum;
}

// Every node named by a bone in any mesh, mapped to its offset (inverse bind) matrix.
std::map<std::string, aiMatrix4x4> collect_bone_offsets(const aiScene* scene) {
    std::map<std::string, aiMatrix4x4> offsets;
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
            const aiBone* bone = mesh->mBones[b];
            offsets.emplace(bone->mName.C_Str(), bone->mOffsetMatrix);
        }
    }
    return offsets;
}

// Marks the nodes that have to become bones: the bones themselves, plus every
// ancestor linking them back to the root. The intermediate nodes usually carry no
// weights, but their transforms are links in the chain - skipping them detaches the
// limb from the body.
bool mark_skeleton_nodes(const aiNode* node, const std::map<std::string, aiMatrix4x4>& bone_offsets,
                          std::set<const aiNode*>& out_needed) {
    bool needed = bone_offsets.count(node->mName.C_Str()) > 0;
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        if (mark_skeleton_nodes(node->mChildren[i], bone_offsets, out_needed)) {
            needed = true;
        }
    }
    if (needed) out_needed.insert(node);
    return needed;
}

// Flattens the marked nodes into Skeleton::bones. The recursion is depth-first from
// the root, so a parent is always appended before its children - which is exactly
// the ordering AnimationPlayer relies on to accumulate globals in a single pass.
void build_skeleton(const aiNode* node, int parent_index, const std::set<const aiNode*>& needed,
                     const std::map<std::string, aiMatrix4x4>& bone_offsets, Skeleton& out_skeleton) {
    int this_index = parent_index;
    if (needed.count(node) > 0) {
        Bone bone;
        bone.name = node->mName.C_Str();
        bone.parent_index = parent_index;
        bone.local_bind_transform = to_matrix4x4(node->mTransformation);

        auto it = bone_offsets.find(bone.name);
        // A pure link node has no offset matrix; identity is correct for it, since
        // nothing is ever weighted to it and it only contributes to its children.
        bone.inverse_bind_pose = (it != bone_offsets.end()) ? to_matrix4x4(it->second) : Matrix4x4::identity();

        this_index = static_cast<int>(out_skeleton.bones.size());
        out_skeleton.bones.push_back(std::move(bone));
    }

    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        build_skeleton(node->mChildren[i], this_index, needed, bone_offsets, out_skeleton);
    }
}

// Converts Assimp's animations, dropping channels that target nodes outside the
// skeleton (exporters routinely animate cameras and helper nulls alongside the rig).
std::vector<AnimationClip> build_animation_clips(const aiScene* scene, const Skeleton& skeleton) {
    std::vector<AnimationClip> clips;
    clips.reserve(scene->mNumAnimations);

    for (unsigned int a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation* anim = scene->mAnimations[a];

        AnimationClip clip;
        clip.name = anim->mName.length > 0 ? anim->mName.C_Str() : ("Clip " + std::to_string(a));
        clip.duration = static_cast<float>(anim->mDuration);
        // Assimp reports 0 when the format does not state a rate; 25 is its own
        // documented assumption for those files.
        clip.ticks_per_second = anim->mTicksPerSecond != 0.0 ? static_cast<float>(anim->mTicksPerSecond) : 25.0f;

        for (unsigned int c = 0; c < anim->mNumChannels; ++c) {
            const aiNodeAnim* node_anim = anim->mChannels[c];
            int bone_index = skeleton.find_bone(node_anim->mNodeName.C_Str());
            if (bone_index < 0) continue;

            BoneChannel channel;
            channel.bone_index = bone_index;

            channel.pos_keys.reserve(node_anim->mNumPositionKeys);
            for (unsigned int k = 0; k < node_anim->mNumPositionKeys; ++k) {
                channel.pos_keys.push_back({ static_cast<float>(node_anim->mPositionKeys[k].mTime),
                                             to_vector3(node_anim->mPositionKeys[k].mValue) });
            }
            channel.rot_keys.reserve(node_anim->mNumRotationKeys);
            for (unsigned int k = 0; k < node_anim->mNumRotationKeys; ++k) {
                channel.rot_keys.push_back({ static_cast<float>(node_anim->mRotationKeys[k].mTime),
                                             to_quaternion(node_anim->mRotationKeys[k].mValue) });
            }
            channel.scale_keys.reserve(node_anim->mNumScalingKeys);
            for (unsigned int k = 0; k < node_anim->mNumScalingKeys; ++k) {
                channel.scale_keys.push_back({ static_cast<float>(node_anim->mScalingKeys[k].mTime),
                                              to_vector3(node_anim->mScalingKeys[k].mValue) });
            }

            if (!channel.pos_keys.empty() || !channel.rot_keys.empty() || !channel.scale_keys.empty()) {
                clip.channels.push_back(std::move(channel));
            }
        }

        if (!clip.channels.empty()) {
            clips.push_back(std::move(clip));
        }
    }
    return clips;
}

// Writes an embedded Assimp texture out to Content/Textures/ and returns the
// path written to, or an empty string on failure.
std::string write_embedded_texture(const aiTexture* tex, const std::string& base_name, int index) {
    std::filesystem::create_directories("Content/Textures");

    if (tex->mHeight == 0) {
        // Already compressed (PNG/JPG/...) - the raw bytes are the file contents.
        std::string ext = tex->achFormatHint[0] ? tex->achFormatHint : "png";
        // achFormatHint is sometimes like "jpg" or "png", occasionally prefixed with a dot.
        if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
        std::string out_path = "Content/Textures/" + base_name + "_embedded" + std::to_string(index) + "." + ext;
        std::ofstream file(out_path, std::ios::binary);
        if (!file) return "";
        file.write(reinterpret_cast<const char*>(tex->pcData), tex->mWidth);
        return out_path;
    }

    // Uncompressed: mWidth x mHeight texels in BGRA order (aiTexel).
    std::vector<unsigned char> rgba(static_cast<size_t>(tex->mWidth) * tex->mHeight * 4);
    for (unsigned int i = 0; i < tex->mWidth * tex->mHeight; i++) {
        const aiTexel& texel = tex->pcData[i];
        rgba[i * 4 + 0] = texel.r;
        rgba[i * 4 + 1] = texel.g;
        rgba[i * 4 + 2] = texel.b;
        rgba[i * 4 + 3] = texel.a;
    }
    std::string out_path = "Content/Textures/" + base_name + "_embedded" + std::to_string(index) + ".png";
    if (!stbi_write_png(out_path.c_str(), tex->mWidth, tex->mHeight, 4, rgba.data(), tex->mWidth * 4)) {
        return "";
    }
    return out_path;
}

// Copies an external texture referenced by the model into Content/Textures/
// and returns the path copied to, or an empty string if it couldn't be found.
std::string copy_external_texture(const std::string& tex_path, const std::string& model_dir) {
    std::filesystem::path candidate(tex_path);
    if (!candidate.is_absolute()) {
        candidate = std::filesystem::path(model_dir) / candidate;
    }
    if (!std::filesystem::exists(candidate)) {
        // Some exporters (FBX in particular) embed absolute paths from the
        // original authoring machine; fall back to just the filename next to the model.
        candidate = std::filesystem::path(model_dir) / std::filesystem::path(tex_path).filename();
    }
    if (!std::filesystem::exists(candidate)) {
        std::cerr << "[ModelImporter] Referenced texture not found: " << tex_path << std::endl;
        return "";
    }

    std::filesystem::create_directories("Content/Textures");
    std::filesystem::path dest = std::filesystem::path("Content/Textures") / candidate.filename();
    std::error_code ec;
    std::filesystem::copy_file(candidate, dest, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[ModelImporter] Failed to copy texture " << candidate << ": " << ec.message() << std::endl;
        return "";
    }
    return dest.generic_string();
}

// Tries to resolve a diffuse/base-color texture from a material. Returns
// true and fills out_texture_path if one was found and successfully written
// into Content/.
bool resolve_material_texture(const aiMaterial* material, const aiScene* scene, const std::string& model_dir,
                               const std::string& base_name, int embedded_index, std::string& out_texture_path) {
    static const aiTextureType kCandidateTypes[] = { aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE };

    for (aiTextureType type : kCandidateTypes) {
        if (material->GetTextureCount(type) == 0) continue;

        aiString tex_path;
        if (material->GetTexture(type, 0, &tex_path) != AI_SUCCESS) continue;

        std::string path_str = tex_path.C_Str();
        if (path_str.empty()) continue;

        if (const aiTexture* embedded = scene->GetEmbeddedTexture(path_str.c_str())) {
            out_texture_path = write_embedded_texture(embedded, base_name, embedded_index);
        } else if (path_str[0] == '*') {
            // Indexed embedded texture (e.g. "*0").
            int idx = std::atoi(path_str.c_str() + 1);
            if (idx >= 0 && static_cast<unsigned int>(idx) < scene->mNumTextures) {
                out_texture_path = write_embedded_texture(scene->mTextures[idx], base_name, embedded_index);
            }
        } else {
            out_texture_path = copy_external_texture(path_str, model_dir);
        }

        if (!out_texture_path.empty()) return true;
    }
    return false;
}

Vector3 resolve_material_color(const aiMaterial* material) {
    aiColor4D color;
    if (aiGetMaterialColor(material, AI_MATKEY_BASE_COLOR, &color) == AI_SUCCESS ||
        aiGetMaterialColor(material, AI_MATKEY_COLOR_DIFFUSE, &color) == AI_SUCCESS) {
        return { color.r, color.g, color.b };
    }
    return { 0.8f, 0.8f, 0.8f };
}

void process_mesh(const aiMesh* mesh, const aiScene* scene, const aiMatrix4x4& world_transform,
                   const std::string& model_dir, const std::string& base_name, int mesh_index,
                   std::vector<Vertex>& out_vertices, std::vector<unsigned int>& out_indices,
                   std::string& out_texture_path, bool& texture_resolved,
                   const Skeleton& skeleton, std::vector<VertexBoneData>& out_bone_data) {
    const bool is_skinned = mesh->mNumBones > 0 && !skeleton.empty();

    // A skinned mesh's vertices must stay in the space its bones' offset matrices
    // were authored against, which is the mesh's own space - the node transform is
    // already accounted for by walking the bone hierarchy at pose time. Baking it in
    // here as well (as the static path does, and must) applies it twice and turns
    // the character inside out the moment it is posed.
    const aiMatrix4x4 mesh_transform = is_skinned ? aiMatrix4x4() : world_transform;

    aiMatrix3x3 normal_matrix(mesh_transform);
    normal_matrix.Inverse().Transpose();

    Vector3 material_color = { 0.8f, 0.8f, 0.8f };
    const aiMaterial* material = nullptr;
    if (mesh->mMaterialIndex < scene->mNumMaterials) {
        material = scene->mMaterials[mesh->mMaterialIndex];
        material_color = resolve_material_color(material);
    }

    if (material && !texture_resolved) {
        texture_resolved = resolve_material_texture(material, scene, model_dir, base_name, mesh_index, out_texture_path);
    }

    unsigned int base_vertex = static_cast<unsigned int>(out_vertices.size());

    for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
        Vertex vertex;

        aiVector3D pos = mesh_transform * mesh->mVertices[i];
        vertex.position = to_vector3(pos);

        if (mesh->HasNormals()) {
            aiVector3D n = normal_matrix * mesh->mNormals[i];
            vertex.normal = to_vector3(n).normalized();
        } else {
            vertex.normal = { 0.0f, 1.0f, 0.0f };
        }

        if (mesh->HasTextureCoords(0)) {
            vertex.uv = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };
        } else {
            vertex.uv = { 0.0f, 0.0f };
        }

        vertex.color = material_color;
        out_vertices.push_back(vertex);
    }

    for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
        const aiFace& face = mesh->mFaces[i];
        for (unsigned int j = 0; j < face.mNumIndices; j++) {
            out_indices.push_back(base_vertex + face.mIndices[j]);
        }
    }

    // Keep the skinning stream the same length as the vertex stream even for the
    // static meshes in a model, so the two stay index-aligned across the merge. The
    // padding entries have zero weight, which the vertex shader reads as "leave this
    // vertex where it is".
    if (!skeleton.empty()) {
        out_bone_data.resize(out_vertices.size());
    }

    if (!is_skinned) return;

    for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
        const aiBone* bone = mesh->mBones[b];
        int bone_index = skeleton.find_bone(bone->mName.C_Str());
        if (bone_index < 0) continue;

        for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
            const aiVertexWeight& weight = bone->mWeights[w];
            size_t target = base_vertex + weight.mVertexId;
            if (target < out_bone_data.size()) {
                add_bone_influence(out_bone_data[target], bone_index, weight.mWeight);
            }
        }
    }

    for (size_t i = base_vertex; i < out_bone_data.size(); ++i) {
        normalize_bone_weights(out_bone_data[i]);
    }
}

void process_node(const aiNode* node, const aiScene* scene, const aiMatrix4x4& parent_transform,
                   const std::string& model_dir, const std::string& base_name,
                   std::vector<Vertex>& out_vertices, std::vector<unsigned int>& out_indices,
                   std::string& out_texture_path, bool& texture_resolved,
                   const Skeleton& skeleton, std::vector<VertexBoneData>& out_bone_data) {
    aiMatrix4x4 world_transform = parent_transform * node->mTransformation;

    for (unsigned int i = 0; i < node->mNumMeshes; i++) {
        const aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        process_mesh(mesh, scene, world_transform, model_dir, base_name, static_cast<int>(node->mMeshes[i]),
                     out_vertices, out_indices, out_texture_path, texture_resolved, skeleton, out_bone_data);
    }

    for (unsigned int i = 0; i < node->mNumChildren; i++) {
        process_node(node->mChildren[i], scene, world_transform, model_dir, base_name,
                     out_vertices, out_indices, out_texture_path, texture_resolved, skeleton, out_bone_data);
    }
}

// ---- Binary helpers for the skin block ----

template <typename T>
void write_pod(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool read_pod(std::ifstream& file, T& value) {
    file.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(file);
}

void write_string(std::ofstream& file, const std::string& str) {
    uint32_t length = static_cast<uint32_t>(str.size());
    write_pod(file, length);
    if (length > 0) file.write(str.data(), length);
}

bool read_string(std::ifstream& file, std::string& out) {
    uint32_t length = 0;
    if (!read_pod(file, length)) return false;
    out.assign(length, '\0');
    if (length > 0) file.read(out.data(), length);
    return static_cast<bool>(file);
}

template <typename T>
void write_vector(std::ofstream& file, const std::vector<T>& values) {
    uint32_t count = static_cast<uint32_t>(values.size());
    write_pod(file, count);
    if (count > 0) file.write(reinterpret_cast<const char*>(values.data()), count * sizeof(T));
}

template <typename T>
bool read_vector(std::ifstream& file, std::vector<T>& out) {
    uint32_t count = 0;
    if (!read_pod(file, count)) return false;
    out.resize(count);
    if (count > 0) file.read(reinterpret_cast<char*>(out.data()), count * sizeof(T));
    return static_cast<bool>(file);
}

} // namespace

std::string ModelImporter::import_model(const std::string& filepath) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(filepath,
        aiProcess_Triangulate |
        aiProcess_GenSmoothNormals |
        aiProcess_FlipUVs |
        aiProcess_CalcTangentSpace |
        // Caps each vertex at four influences, matching VertexBoneData. Without it
        // a densely-rigged model silently loses its smallest weights unnormalised.
        aiProcess_LimitBoneWeights |
        aiProcess_JoinIdenticalVertices);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
        std::cerr << "[ModelImporter] Assimp error importing '" << filepath << "': " << importer.GetErrorString() << std::endl;
        return "";
    }

    if (scene->mNumMeshes == 0) {
        std::cerr << "[ModelImporter] '" << filepath << "' has no meshes!" << std::endl;
        return "";
    }

    std::filesystem::path source_path(filepath);
    std::string model_dir = source_path.parent_path().string();
    std::string base_name = source_path.stem().string();

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::string texture_path;
    bool texture_resolved = false;

    // The skeleton has to exist before any mesh is processed: process_mesh resolves
    // each bone's name to an index through it, and also decides whether to bake the
    // node transform based on whether a skeleton was found at all.
    Skeleton skeleton;
    std::map<std::string, aiMatrix4x4> bone_offsets = collect_bone_offsets(scene);
    if (!bone_offsets.empty()) {
        std::set<const aiNode*> needed;
        mark_skeleton_nodes(scene->mRootNode, bone_offsets, needed);
        build_skeleton(scene->mRootNode, -1, needed, bone_offsets, skeleton);
        skeleton.global_inverse_transform = to_matrix4x4(scene->mRootNode->mTransformation).inverse();

        if (skeleton.bone_count() > kMaxSkeletonBones) {
            std::cerr << "[ModelImporter] '" << filepath << "' has " << skeleton.bone_count()
                      << " bones, over the " << kMaxSkeletonBones
                      << "-bone shader limit; importing as a static mesh instead." << std::endl;
            skeleton.bones.clear();
        }
    }

    std::vector<VertexBoneData> bone_data;
    std::vector<AnimationClip> clips;

    process_node(scene->mRootNode, scene, aiMatrix4x4(), model_dir, base_name,
                 vertices, indices, texture_path, texture_resolved, skeleton, bone_data);

    if (vertices.empty() || indices.empty()) {
        std::cerr << "[ModelImporter] '" << filepath << "' produced no geometry!" << std::endl;
        return "";
    }

    if (!skeleton.empty()) {
        bone_data.resize(vertices.size());
        clips = build_animation_clips(scene, skeleton);
    }

    std::string out_path = "Content/" + base_name + ".mesh";
    if (!std::filesystem::exists("Content")) {
        std::filesystem::create_directory("Content");
    }

    std::ofstream file(out_path, std::ios::out | std::ios::binary);
    if (!file) {
        std::cerr << "[ModelImporter] Failed to write .mesh file: " << out_path << std::endl;
        return "";
    }

    uint32_t magic = MESH_FILE_MAGIC;
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));

    size_t num_vertices = vertices.size();
    file.write(reinterpret_cast<const char*>(&num_vertices), sizeof(size_t));
    file.write(reinterpret_cast<const char*>(vertices.data()), num_vertices * sizeof(Vertex));

    size_t num_indices = indices.size();
    file.write(reinterpret_cast<const char*>(&num_indices), sizeof(size_t));
    file.write(reinterpret_cast<const char*>(indices.data()), num_indices * sizeof(unsigned int));

    uint32_t texture_path_len = static_cast<uint32_t>(texture_path.size());
    file.write(reinterpret_cast<const char*>(&texture_path_len), sizeof(texture_path_len));
    if (texture_path_len > 0) {
        file.write(texture_path.data(), texture_path_len);
    }

    // Skin block, appended only when there is one. Everything above is byte-for-byte
    // what previous versions wrote, so a static mesh imported now is still readable
    // by anything that predates this and vice versa.
    if (!skeleton.empty()) {
        write_pod(file, SKIN_BLOCK_MAGIC);
        write_pod(file, SKIN_BLOCK_VERSION);

        uint32_t bone_count = static_cast<uint32_t>(skeleton.bones.size());
        write_pod(file, bone_count);
        for (const Bone& bone : skeleton.bones) {
            write_string(file, bone.name);
            int32_t parent = static_cast<int32_t>(bone.parent_index);
            write_pod(file, parent);
            write_pod(file, bone.inverse_bind_pose);
            write_pod(file, bone.local_bind_transform);
        }
        write_pod(file, skeleton.global_inverse_transform);

        write_vector(file, bone_data);

        uint32_t clip_count = static_cast<uint32_t>(clips.size());
        write_pod(file, clip_count);
        for (const AnimationClip& clip : clips) {
            write_string(file, clip.name);
            write_pod(file, clip.duration);
            write_pod(file, clip.ticks_per_second);

            uint32_t channel_count = static_cast<uint32_t>(clip.channels.size());
            write_pod(file, channel_count);
            for (const BoneChannel& channel : clip.channels) {
                int32_t bone_index = static_cast<int32_t>(channel.bone_index);
                write_pod(file, bone_index);
                write_vector(file, channel.pos_keys);
                write_vector(file, channel.rot_keys);
                write_vector(file, channel.scale_keys);
            }
        }
    }

    if (!file) {
        std::cerr << "[ModelImporter] Error while writing .mesh file: " << out_path << std::endl;
        return "";
    }

    std::cout << "[ModelImporter] Imported '" << filepath << "' -> '" << out_path << "' ("
              << num_vertices << " vertices, " << num_indices << " indices, "
              << scene->mNumMeshes << " source mesh(es)"
              << (texture_path.empty() ? "" : ", texture: " + texture_path);
    if (!skeleton.empty()) {
        std::cout << ", skeleton: " << skeleton.bone_count() << " bones, "
                  << clips.size() << " animation(s)";
    }
    std::cout << ")" << std::endl;
    return out_path;
}

bool ModelImporter::write_static_mesh_file(const std::string& filepath,
                                           const std::vector<Vertex>& vertices,
                                           const std::vector<unsigned int>& indices,
                                           const std::string& texture_path) {
    if (vertices.empty() || indices.empty()) return false;

    std::error_code ec;
    std::filesystem::path parent = std::filesystem::path(filepath).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    std::ofstream file(filepath, std::ios::out | std::ios::binary);
    if (!file) {
        std::cerr << "[ModelImporter] Failed to write .mesh file: " << filepath << std::endl;
        return false;
    }

    // Byte-for-byte the layout import_model writes, up to and including the texture
    // path. Stopping there is a complete, valid static mesh: the reader treats the
    // absence of a skin block as "not skinned", not as a truncated file.
    uint32_t magic = MESH_FILE_MAGIC;
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));

    size_t num_vertices = vertices.size();
    file.write(reinterpret_cast<const char*>(&num_vertices), sizeof(size_t));
    file.write(reinterpret_cast<const char*>(vertices.data()), num_vertices * sizeof(Vertex));

    size_t num_indices = indices.size();
    file.write(reinterpret_cast<const char*>(&num_indices), sizeof(size_t));
    file.write(reinterpret_cast<const char*>(indices.data()), num_indices * sizeof(unsigned int));

    uint32_t texture_path_len = static_cast<uint32_t>(texture_path.size());
    file.write(reinterpret_cast<const char*>(&texture_path_len), sizeof(texture_path_len));
    if (texture_path_len > 0) {
        file.write(texture_path.data(), texture_path_len);
    }

    return static_cast<bool>(file);
}

bool ModelImporter::load_mesh_file(const std::string& filepath, std::vector<Vertex>& out_vertices,
                                    std::vector<unsigned int>& out_indices, std::string& out_texture_path,
                                    Skeleton* out_skeleton, std::vector<AnimationClip>* out_clips,
                                    std::vector<VertexBoneData>* out_bone_data) {
    std::ifstream file(filepath, std::ios::in | std::ios::binary);
    if (!file) {
        std::cerr << "[ModelImporter] Failed to open .mesh file: " << filepath << std::endl;
        return false;
    }

    uint32_t magic = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!file || magic != MESH_FILE_MAGIC) {
        std::cerr << "[ModelImporter] '" << filepath << "' is not a valid .mesh file (bad magic)." << std::endl;
        return false;
    }

    size_t num_vertices = 0;
    file.read(reinterpret_cast<char*>(&num_vertices), sizeof(size_t));
    out_vertices.resize(num_vertices);
    file.read(reinterpret_cast<char*>(out_vertices.data()), num_vertices * sizeof(Vertex));

    size_t num_indices = 0;
    file.read(reinterpret_cast<char*>(&num_indices), sizeof(size_t));
    out_indices.resize(num_indices);
    file.read(reinterpret_cast<char*>(out_indices.data()), num_indices * sizeof(unsigned int));

    uint32_t texture_path_len = 0;
    file.read(reinterpret_cast<char*>(&texture_path_len), sizeof(texture_path_len));
    out_texture_path.clear();
    if (texture_path_len > 0) {
        out_texture_path.resize(texture_path_len);
        file.read(out_texture_path.data(), texture_path_len);
    }

    if (!file && !file.eof()) {
        std::cerr << "[ModelImporter] Error while reading .mesh file: " << filepath << std::endl;
        return false;
    }

    if (out_skeleton) out_skeleton->bones.clear();
    if (out_clips) out_clips->clear();
    if (out_bone_data) out_bone_data->clear();

    // Optional skin block. Hitting end-of-file here is the normal outcome for a
    // static mesh, so failure to read the marker is not an error - only a marker
    // that is present but wrong is.
    uint32_t skin_magic = 0;
    if (!read_pod(file, skin_magic)) {
        return true;
    }
    if (skin_magic != SKIN_BLOCK_MAGIC) {
        std::cerr << "[ModelImporter] '" << filepath << "' has trailing data that is not a skin block; ignoring it."
                  << std::endl;
        return true;
    }

    uint32_t version = 0;
    if (!read_pod(file, version) || version != SKIN_BLOCK_VERSION) {
        std::cerr << "[ModelImporter] '" << filepath << "' has skin block version " << version
                  << ", expected " << SKIN_BLOCK_VERSION << "; re-import the source model." << std::endl;
        return true;
    }

    // The caller may not want skinning at all (the path tracer only needs geometry).
    // The block still has to parse correctly to be reported, but there is nowhere to
    // put it, so stop here.
    if (!out_skeleton || !out_clips || !out_bone_data) {
        return true;
    }

    Skeleton skeleton;
    uint32_t bone_count = 0;
    if (!read_pod(file, bone_count)) return true;
    skeleton.bones.resize(bone_count);
    for (uint32_t i = 0; i < bone_count; ++i) {
        Bone& bone = skeleton.bones[i];
        int32_t parent = -1;
        if (!read_string(file, bone.name) ||
            !read_pod(file, parent) ||
            !read_pod(file, bone.inverse_bind_pose) ||
            !read_pod(file, bone.local_bind_transform)) {
            std::cerr << "[ModelImporter] Truncated skeleton in '" << filepath << "'." << std::endl;
            return true;
        }
        bone.parent_index = parent;
    }
    if (!read_pod(file, skeleton.global_inverse_transform)) return true;

    std::vector<VertexBoneData> bone_data;
    if (!read_vector(file, bone_data)) {
        std::cerr << "[ModelImporter] Truncated skin weights in '" << filepath << "'." << std::endl;
        return true;
    }

    std::vector<AnimationClip> clips;
    uint32_t clip_count = 0;
    if (!read_pod(file, clip_count)) return true;
    clips.resize(clip_count);
    for (uint32_t i = 0; i < clip_count; ++i) {
        AnimationClip& clip = clips[i];
        if (!read_string(file, clip.name) ||
            !read_pod(file, clip.duration) ||
            !read_pod(file, clip.ticks_per_second)) {
            std::cerr << "[ModelImporter] Truncated animation in '" << filepath << "'." << std::endl;
            clips.resize(i);
            break;
        }

        uint32_t channel_count = 0;
        if (!read_pod(file, channel_count)) { clips.resize(i); break; }
        clip.channels.resize(channel_count);
        bool channels_ok = true;
        for (uint32_t c = 0; c < channel_count; ++c) {
            BoneChannel& channel = clip.channels[c];
            int32_t bone_index = -1;
            if (!read_pod(file, bone_index) ||
                !read_vector(file, channel.pos_keys) ||
                !read_vector(file, channel.rot_keys) ||
                !read_vector(file, channel.scale_keys)) {
                channels_ok = false;
                break;
            }
            channel.bone_index = bone_index;
        }
        if (!channels_ok) {
            std::cerr << "[ModelImporter] Truncated animation channels in '" << filepath << "'." << std::endl;
            clips.resize(i);
            break;
        }
    }

    *out_skeleton = std::move(skeleton);
    *out_bone_data = std::move(bone_data);
    *out_clips = std::move(clips);
    return true;
}
