#pragma once

#include "world/static_mesh_component.hpp"
#include <string>
#include <vector>

// Builds reduced copies of a mesh for an LOD group.
//
// LOD levels are normally authored, but a project that has none has no way to use
// an LOD group at all - so the editor can generate them, and this is what it calls.
//
// The method is quadric-weighted vertex clustering: the mesh is divided by a
// uniform grid, every vertex in a cell collapses to one representative, and
// triangles whose corners end up in the same cell disappear. The representative is
// the point minimising the summed squared distance to the planes of the original
// triangles, which is what keeps hard edges and flat faces where they were instead
// of rounding a cube's corners off.
//
// Clustering rather than iterative edge collapse because it is single-pass and
// cannot get into a bad state: any input, however broken its topology, produces a
// mesh with valid indices or produces nothing.
namespace MeshSimplifier {

// Reduces to roughly target_ratio of the input triangle count (0 < ratio < 1).
// Returns false if the input is unusable or the reduction produced no triangles,
// leaving the outputs untouched.
bool simplify(const std::vector<Vertex>& in_vertices,
              const std::vector<unsigned int>& in_indices,
              float target_ratio,
              std::vector<Vertex>& out_vertices,
              std::vector<unsigned int>& out_indices);

// Generates `level_count` progressively coarser .mesh files beside `source_mesh_path`,
// named "<source>_LOD1.mesh", "<source>_LOD2.mesh" and so on. Each level keeps
// `ratio_per_level` of the previous level's triangles.
//
// Returns the paths written, shortest chain first. Empty on failure, with the reason
// in out_error.
std::vector<std::string> generate_lod_chain(const std::string& source_mesh_path,
                                            int level_count,
                                            float ratio_per_level,
                                            std::string& out_error);

} // namespace MeshSimplifier
