#pragma once

#include "core/math.hpp"
#include <string>
#include <vector>

// A single bone in the skeleton hierarchy
struct Bone {
    std::string name;
    int parent_index = -1; // -1 = root bone
    Matrix4x4 inverse_bind_pose; // Transforms from mesh-space to bone-local space
    // The bone's own transform in the imported rest pose, relative to its parent.
    // Used verbatim whenever a clip has no channel targeting this bone: a clip that
    // only animates the arms still has to place the legs somewhere, and the rest
    // pose is the only correct answer. Without it those bones collapse onto their
    // parent and the mesh folds into the origin.
    Matrix4x4 local_bind_transform;
};

// The full skeleton: ordered list of bones where child bones appear after their parents
class Skeleton {
public:
    std::vector<Bone> bones;

    // Undoes whatever transform the model's root node carries (unit conversion,
    // axis correction, an exporter's root offset). Bone globals are accumulated in
    // the file's own node space, so this maps them back into mesh space, which is
    // the space the vertices are in.
    Matrix4x4 global_inverse_transform;

    // Returns bone index by name, or -1 if not found
    int find_bone(const std::string& name) const {
        for (int i = 0; i < (int)bones.size(); ++i) {
            if (bones[i].name == name) return i;
        }
        return -1;
    }

    int bone_count() const { return (int)bones.size(); }
    bool empty() const { return bones.empty(); }
};
