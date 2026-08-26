#pragma once

#include <vector>
#include <cstdint>
#include "core/math_utils.hpp" // Assuming we have Vector3 somewhere, we'll just use a generic struct if not

namespace Rendering {

// Structure for a Mesh Signed Distance Field (SDF)
// Used for software ray-marching to calculate Global Illumination and shadows without hardware RT cores.

struct Vector3 { float x, y, z; };

class SDFVolume {
public:
    SDFVolume(uint32_t resolution_x, uint32_t resolution_y, uint32_t resolution_z)
        : res_x(resolution_x), res_y(resolution_y), res_z(resolution_z) {
        distances.resize(res_x * res_y * res_z, 0.0f);
    }

    // Generate the SDF from a triangle mesh (heavy operation, usually done offline or baked)
    void generate_from_mesh(const std::vector<Vector3>& vertices, const std::vector<uint32_t>& indices) {
        // Compute signed distance for each voxel
    }

    // Upload the 3D texture to the GPU for the compute shader to ray-march against
    void upload_to_gpu() {
        // RHI Call to create Texture3D
    }

private:
    uint32_t res_x, res_y, res_z;
    std::vector<float> distances; // Positive = outside, Negative = inside mesh
};

// Global SDF manages a low-res SDF of the entire scene for distant GI
class GlobalSDF {
public:
    void merge_mesh_sdf(const SDFVolume& mesh_sdf, const Vector3& position) {
        // Combine a high-res mesh SDF into the global scene SDF via compute shader min() operation
    }
};

} // namespace Rendering
