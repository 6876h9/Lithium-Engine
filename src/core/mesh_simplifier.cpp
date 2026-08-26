#include "core/mesh_simplifier.hpp"
#include "core/model_importer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <unordered_map>

namespace {

// A symmetric 4x4 error quadric, stored as its ten distinct entries. Adding the
// quadrics of the planes a vertex touches gives a matrix whose value at a point is
// the summed squared distance to those planes - the standard measure of how much
// moving a vertex distorts the surface.
struct Quadric {
    double q[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    // index: 0=aa 1=ab 2=ac 3=ad 4=bb 5=bc 6=bd 7=cc 8=cd 9=dd

    void add_plane(double a, double b, double c, double d, double weight) {
        q[0] += weight * a * a; q[1] += weight * a * b; q[2] += weight * a * c; q[3] += weight * a * d;
        q[4] += weight * b * b; q[5] += weight * b * c; q[6] += weight * b * d;
        q[7] += weight * c * c; q[8] += weight * c * d;
        q[9] += weight * d * d;
    }

    void add(const Quadric& other) {
        for (int i = 0; i < 10; ++i) q[i] += other.q[i];
    }

    // Point minimising this quadric, by solving the 3x3 system its gradient gives.
    // Returns false when the system is singular, which happens whenever the planes
    // do not pin the point down in all three directions - a flat sheet, a straight
    // edge - and the caller has to fall back to the cluster average.
    bool minimiser(double& out_x, double& out_y, double& out_z) const {
        const double m00 = q[0], m01 = q[1], m02 = q[2];
        const double m11 = q[4], m12 = q[5];
        const double m22 = q[7];

        const double det =
            m00 * (m11 * m22 - m12 * m12) -
            m01 * (m01 * m22 - m12 * m02) +
            m02 * (m01 * m12 - m11 * m02);

        // Scaled against the matrix magnitude: an absolute epsilon would call a
        // perfectly solvable system singular on a mesh authored in millimetres.
        const double magnitude = std::abs(m00) + std::abs(m11) + std::abs(m22) + 1e-30;
        if (std::abs(det) < 1e-10 * magnitude * magnitude * magnitude) return false;

        const double bx = -q[3], by = -q[6], bz = -q[8];
        const double inv_det = 1.0 / det;

        out_x = inv_det * (bx * (m11 * m22 - m12 * m12) -
                           m01 * (by * m22 - m12 * bz) +
                           m02 * (by * m12 - m11 * bz));
        out_y = inv_det * (m00 * (by * m22 - m12 * bz) -
                           bx  * (m01 * m22 - m12 * m02) +
                           m02 * (m01 * bz - by * m02));
        out_z = inv_det * (m00 * (m11 * bz - by * m12) -
                           m01 * (m01 * bz - by * m02) +
                           bx  * (m01 * m12 - m11 * m02));
        return true;
    }
};

struct Cluster {
    Quadric quadric;
    double sum_x = 0.0, sum_y = 0.0, sum_z = 0.0;
    double sum_nx = 0.0, sum_ny = 0.0, sum_nz = 0.0;
    double sum_u = 0.0, sum_v = 0.0;
    double sum_cr = 0.0, sum_cg = 0.0, sum_cb = 0.0;
    int count = 0;
    // Cell bounds, used to clamp the quadric minimiser. A near-singular system can
    // place the optimum far outside the cell it came from, which reads as a spike
    // shooting out of the model.
    double min_x = 0, min_y = 0, min_z = 0;
    double max_x = 0, max_y = 0, max_z = 0;
    int output_index = -1;
};

struct CellKey {
    int x, y, z;
    bool operator==(const CellKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct CellKeyHash {
    size_t operator()(const CellKey& k) const {
        // Three large odd constants; mixing by multiply-xor keeps neighbouring
        // cells - which is what a mesh produces - from landing in the same bucket.
        size_t h = static_cast<size_t>(k.x) * 73856093u;
        h ^= static_cast<size_t>(k.y) * 19349663u;
        h ^= static_cast<size_t>(k.z) * 83492791u;
        return h;
    }
};

} // namespace

namespace MeshSimplifier {

bool simplify(const std::vector<Vertex>& in_vertices,
              const std::vector<unsigned int>& in_indices,
              float target_ratio,
              std::vector<Vertex>& out_vertices,
              std::vector<unsigned int>& out_indices) {
    if (in_vertices.empty() || in_indices.size() < 3) return false;
    if (!(target_ratio > 0.0f) || target_ratio >= 1.0f) return false;

    // --- Bounds and grid size ---------------------------------------------
    Vector3 bounds_min = in_vertices[0].position;
    Vector3 bounds_max = in_vertices[0].position;
    for (const Vertex& v : in_vertices) {
        bounds_min.x = std::min(bounds_min.x, v.position.x);
        bounds_min.y = std::min(bounds_min.y, v.position.y);
        bounds_min.z = std::min(bounds_min.z, v.position.z);
        bounds_max.x = std::max(bounds_max.x, v.position.x);
        bounds_max.y = std::max(bounds_max.y, v.position.y);
        bounds_max.z = std::max(bounds_max.z, v.position.z);
    }

    double ex = std::max(1e-5, static_cast<double>(bounds_max.x - bounds_min.x));
    double ey = std::max(1e-5, static_cast<double>(bounds_max.y - bounds_min.y));
    double ez = std::max(1e-5, static_cast<double>(bounds_max.z - bounds_min.z));

    // Cell size chosen so the grid holds roughly the target number of cells. A flat
    // mesh has one tiny extent, which would make the cube root collapse the whole
    // thing - the max() above is what stops that.
    const double target_cells = std::max(8.0, static_cast<double>(in_vertices.size()) * target_ratio);
    double cell = std::cbrt((ex * ey * ez) / target_cells);
    if (!(cell > 0.0) || !std::isfinite(cell)) return false;

    // Never coarser than a quarter of the smallest *real* extent, or a slab-shaped
    // mesh ends up one cell thick and every triangle in it degenerates. Axes that
    // are flat are excluded: a ground plane has no thickness at all, and clamping to
    // a quarter of that would ask for a grid of billions of cells.
    double smallest_real = 1e30;
    for (double extent : { ex, ey, ez }) {
        if (extent > 1e-3) smallest_real = std::min(smallest_real, extent);
    }
    if (smallest_real < 1e29) cell = std::min(cell, smallest_real * 0.25);

    // And never finer than one cell per input vertex. Clustering can only ever
    // reduce, so a grid larger than the mesh is guaranteed waste - and with a
    // degenerate extent it is unbounded waste.
    const double finest = std::cbrt((ex * ey * ez) / std::max(1.0, static_cast<double>(in_vertices.size())));
    cell = std::max(cell, finest);
    if (!(cell > 0.0) || !std::isfinite(cell)) return false;

    // --- Cluster assignment ------------------------------------------------
    std::unordered_map<CellKey, int, CellKeyHash> cell_to_cluster;
    std::vector<Cluster> clusters;
    std::vector<int> cluster_of_vertex(in_vertices.size(), -1);

    clusters.reserve(static_cast<size_t>(target_cells));
    for (size_t i = 0; i < in_vertices.size(); ++i) {
        const Vector3& p = in_vertices[i].position;
        CellKey key{
            static_cast<int>(std::floor((p.x - bounds_min.x) / cell)),
            static_cast<int>(std::floor((p.y - bounds_min.y) / cell)),
            static_cast<int>(std::floor((p.z - bounds_min.z) / cell))
        };

        auto it = cell_to_cluster.find(key);
        int cluster_index;
        if (it == cell_to_cluster.end()) {
            cluster_index = static_cast<int>(clusters.size());
            cell_to_cluster.emplace(key, cluster_index);
            Cluster c;
            c.min_x = bounds_min.x + key.x * cell;
            c.min_y = bounds_min.y + key.y * cell;
            c.min_z = bounds_min.z + key.z * cell;
            c.max_x = c.min_x + cell;
            c.max_y = c.min_y + cell;
            c.max_z = c.min_z + cell;
            clusters.push_back(c);
        } else {
            cluster_index = it->second;
        }

        cluster_of_vertex[i] = cluster_index;
        Cluster& c = clusters[cluster_index];
        c.sum_x += p.x; c.sum_y += p.y; c.sum_z += p.z;
        c.sum_nx += in_vertices[i].normal.x;
        c.sum_ny += in_vertices[i].normal.y;
        c.sum_nz += in_vertices[i].normal.z;
        c.sum_u += in_vertices[i].uv.x;
        c.sum_v += in_vertices[i].uv.y;
        c.sum_cr += in_vertices[i].color.x;
        c.sum_cg += in_vertices[i].color.y;
        c.sum_cb += in_vertices[i].color.z;
        c.count++;
    }

    // --- Accumulate face quadrics -----------------------------------------
    for (size_t i = 0; i + 2 < in_indices.size(); i += 3) {
        const unsigned int i0 = in_indices[i], i1 = in_indices[i + 1], i2 = in_indices[i + 2];
        if (i0 >= in_vertices.size() || i1 >= in_vertices.size() || i2 >= in_vertices.size()) continue;

        const Vector3& p0 = in_vertices[i0].position;
        const Vector3& p1 = in_vertices[i1].position;
        const Vector3& p2 = in_vertices[i2].position;

        const Vector3 e1 = p1 - p0;
        const Vector3 e2 = p2 - p0;
        Vector3 n = Vector3::cross(e1, e2);
        const double twice_area = n.length();
        if (twice_area < 1e-12) continue; // degenerate triangle contributes no plane

        n = n / static_cast<float>(twice_area);
        const double d = -(static_cast<double>(n.x) * p0.x +
                           static_cast<double>(n.y) * p0.y +
                           static_cast<double>(n.z) * p0.z);

        // Area weighting, so a large flat face constrains its vertices more than a
        // sliver does - otherwise dense detail dominates the shape of the result.
        Quadric face;
        face.add_plane(n.x, n.y, n.z, d, twice_area * 0.5);

        clusters[cluster_of_vertex[i0]].quadric.add(face);
        clusters[cluster_of_vertex[i1]].quadric.add(face);
        clusters[cluster_of_vertex[i2]].quadric.add(face);
    }

    // --- Representative vertices ------------------------------------------
    out_vertices.clear();
    out_vertices.reserve(clusters.size());
    for (Cluster& c : clusters) {
        if (c.count <= 0) continue;

        const double inv = 1.0 / c.count;
        double px = c.sum_x * inv;
        double py = c.sum_y * inv;
        double pz = c.sum_z * inv;

        double ox, oy, oz;
        if (c.quadric.minimiser(ox, oy, oz)) {
            // Clamped to the cell: the optimum of a nearly degenerate quadric can
            // be arbitrarily far away, and a vertex that leaves its own cell tears
            // the surface open.
            if (ox >= c.min_x && ox <= c.max_x &&
                oy >= c.min_y && oy <= c.max_y &&
                oz >= c.min_z && oz <= c.max_z) {
                px = ox; py = oy; pz = oz;
            }
        }

        Vertex v;
        v.position = { static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz) };
        v.color = { static_cast<float>(c.sum_cr * inv),
                    static_cast<float>(c.sum_cg * inv),
                    static_cast<float>(c.sum_cb * inv) };
        Vector3 normal = { static_cast<float>(c.sum_nx * inv),
                           static_cast<float>(c.sum_ny * inv),
                           static_cast<float>(c.sum_nz * inv) };
        v.normal = normal.normalized();
        v.uv = { static_cast<float>(c.sum_u * inv), static_cast<float>(c.sum_v * inv) };

        c.output_index = static_cast<int>(out_vertices.size());
        out_vertices.push_back(v);
    }

    if (out_vertices.empty()) return false;

    // --- Rebuild the index buffer -----------------------------------------
    out_indices.clear();
    out_indices.reserve(in_indices.size());
    for (size_t i = 0; i + 2 < in_indices.size(); i += 3) {
        const unsigned int i0 = in_indices[i], i1 = in_indices[i + 1], i2 = in_indices[i + 2];
        if (i0 >= in_vertices.size() || i1 >= in_vertices.size() || i2 >= in_vertices.size()) continue;

        const int c0 = clusters[cluster_of_vertex[i0]].output_index;
        const int c1 = clusters[cluster_of_vertex[i1]].output_index;
        const int c2 = clusters[cluster_of_vertex[i2]].output_index;
        if (c0 < 0 || c1 < 0 || c2 < 0) continue;
        // Two corners in the same cell means the triangle collapsed to a line. That
        // is the reduction working, not an error.
        if (c0 == c1 || c1 == c2 || c0 == c2) continue;

        out_indices.push_back(static_cast<unsigned int>(c0));
        out_indices.push_back(static_cast<unsigned int>(c1));
        out_indices.push_back(static_cast<unsigned int>(c2));
    }

    if (out_indices.size() < 3) {
        out_vertices.clear();
        out_indices.clear();
        return false;
    }

    // --- Repair degenerate normals ----------------------------------------
    // A cluster whose members' normals cancelled out (the two sides of a thin wall
    // collapsing together) has no usable averaged normal, so take it from the
    // geometry that actually survived.
    std::vector<bool> needs_normal(out_vertices.size(), false);
    bool any_needed = false;
    for (size_t i = 0; i < out_vertices.size(); ++i) {
        if (out_vertices[i].normal.length() < 0.5f) {
            needs_normal[i] = true;
            out_vertices[i].normal = { 0.0f, 0.0f, 0.0f };
            any_needed = true;
        }
    }
    if (any_needed) {
        for (size_t i = 0; i + 2 < out_indices.size(); i += 3) {
            const unsigned int a = out_indices[i], b = out_indices[i + 1], c = out_indices[i + 2];
            const Vector3 face = Vector3::cross(out_vertices[b].position - out_vertices[a].position,
                                                out_vertices[c].position - out_vertices[a].position);
            if (needs_normal[a]) out_vertices[a].normal += face;
            if (needs_normal[b]) out_vertices[b].normal += face;
            if (needs_normal[c]) out_vertices[c].normal += face;
        }
        for (size_t i = 0; i < out_vertices.size(); ++i) {
            if (!needs_normal[i]) continue;
            Vector3 n = out_vertices[i].normal.normalized();
            // Still nothing usable: an isolated vertex with no surviving faces.
            if (n.length() < 0.5f) n = { 0.0f, 1.0f, 0.0f };
            out_vertices[i].normal = n;
        }
    }

    return true;
}

std::vector<std::string> generate_lod_chain(const std::string& source_mesh_path,
                                            int level_count,
                                            float ratio_per_level,
                                            std::string& out_error) {
    std::vector<std::string> written;
    out_error.clear();

    if (level_count <= 0) {
        out_error = "Level count must be at least 1.";
        return written;
    }
    if (!(ratio_per_level > 0.05f) || ratio_per_level >= 1.0f) {
        out_error = "Reduction per level must be between 0.05 and 1.";
        return written;
    }

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::string texture_path;
    if (!ModelImporter::load_mesh_file(source_mesh_path, vertices, indices, texture_path)) {
        out_error = "Could not read " + source_mesh_path;
        return written;
    }
    if (vertices.empty() || indices.size() < 3) {
        out_error = source_mesh_path + " has no geometry to reduce.";
        return written;
    }

    const std::filesystem::path source(source_mesh_path);
    const std::string stem = source.stem().string();
    const std::filesystem::path directory = source.parent_path();

    // Each level reduces the previous level rather than the original, so the chain
    // gets progressively cheaper instead of every level being the same reduction of
    // the same source.
    for (int level = 1; level <= level_count; ++level) {
        std::vector<Vertex> reduced_vertices;
        std::vector<unsigned int> reduced_indices;
        if (!simplify(vertices, indices, ratio_per_level, reduced_vertices, reduced_indices)) {
            // Running out of geometry to remove is a normal end to the chain, not a
            // failure - the levels already written are still valid.
            if (written.empty()) {
                out_error = "Mesh is already too coarse to reduce further.";
            }
            break;
        }

        const std::filesystem::path out_path =
            directory / (stem + "_LOD" + std::to_string(level) + ".mesh");
        if (!ModelImporter::write_static_mesh_file(out_path.string(), reduced_vertices,
                                                   reduced_indices, texture_path)) {
            out_error = "Could not write " + out_path.string();
            break;
        }

        std::cout << "[LOD] " << out_path.string() << ": "
                  << (indices.size() / 3) << " -> " << (reduced_indices.size() / 3)
                  << " triangles" << std::endl;

        written.push_back(out_path.string());
        vertices = std::move(reduced_vertices);
        indices = std::move(reduced_indices);
    }

    return written;
}

} // namespace MeshSimplifier
