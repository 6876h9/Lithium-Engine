#include "renderer/lightmapper.hpp"

#include "world/actor.hpp"
#include "world/static_mesh_component.hpp"
#include "core/mesh_resource.hpp"
#include "renderer/gl_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>

namespace {

constexpr uint32_t kLightmapMagic = 0x4C4D4150; // "LMAP"
constexpr uint32_t kLightmapVersion = 1;

// Gutter around each chart, in texels. Bilinear filtering reaches half a texel past
// a chart's edge, and without a margin it reaches into the neighbouring chart -
// which is what produces the classic lightmap seam.
constexpr int kChartPadding = 2;

float random01(unsigned int& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state & 0x00FFFFFFu) / 16777216.0f;
}

// Cosine-weighted direction about a normal. Cosine weighting is what lets the
// estimator drop the N-dot-L term: the samples are already distributed in
// proportion to it, so each one contributes equally.
Vector3 cosine_hemisphere(const Vector3& normal, unsigned int& state) {
    const float u1 = random01(state);
    const float u2 = random01(state);
    const float r = std::sqrt(u1);
    const float theta = 2.0f * 3.14159265358979323846f * u2;

    const float x = r * std::cos(theta);
    const float y = r * std::sin(theta);
    const float z = std::sqrt(std::max(0.0f, 1.0f - u1));

    // Build a frame around the normal. Picking the helper axis by the smallest
    // component keeps the cross product from collapsing when the normal is close to
    // an axis.
    Vector3 helper = (std::abs(normal.x) < 0.9f) ? Vector3{ 1.0f, 0.0f, 0.0f }
                                                 : Vector3{ 0.0f, 1.0f, 0.0f };
    const Vector3 tangent = Vector3::cross(helper, normal).normalized();
    const Vector3 bitangent = Vector3::cross(normal, tangent);
    return (tangent * x + bitangent * y + normal * z).normalized();
}

bool ray_box(const Vector3& origin, const Vector3& inv_direction,
             const Vector3& bounds_min, const Vector3& bounds_max, float max_distance) {
    const float tx1 = (bounds_min.x - origin.x) * inv_direction.x;
    const float tx2 = (bounds_max.x - origin.x) * inv_direction.x;
    float tmin = std::min(tx1, tx2);
    float tmax = std::max(tx1, tx2);

    const float ty1 = (bounds_min.y - origin.y) * inv_direction.y;
    const float ty2 = (bounds_max.y - origin.y) * inv_direction.y;
    tmin = std::max(tmin, std::min(ty1, ty2));
    tmax = std::min(tmax, std::max(ty1, ty2));

    const float tz1 = (bounds_min.z - origin.z) * inv_direction.z;
    const float tz2 = (bounds_max.z - origin.z) * inv_direction.z;
    tmin = std::max(tmin, std::min(tz1, tz2));
    tmax = std::min(tmax, std::max(tz1, tz2));

    return tmax >= std::max(0.0f, tmin) && tmin <= max_distance;
}

// Moller-Trumbore. Returns the distance, or a negative number for a miss.
float ray_triangle(const Vector3& origin, const Vector3& direction,
                   const Vector3& a, const Vector3& b, const Vector3& c) {
    const Vector3 edge1 = b - a;
    const Vector3 edge2 = c - a;
    const Vector3 h = Vector3::cross(direction, edge2);
    const float determinant = Vector3::dot(edge1, h);
    if (std::abs(determinant) < 1e-8f) return -1.0f;

    const float inv_determinant = 1.0f / determinant;
    const Vector3 s = origin - a;
    const float u = Vector3::dot(s, h) * inv_determinant;
    if (u < 0.0f || u > 1.0f) return -1.0f;

    const Vector3 q = Vector3::cross(s, edge1);
    const float v = Vector3::dot(direction, q) * inv_determinant;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;

    return Vector3::dot(edge2, q) * inv_determinant;
}

} // namespace

Lightmapper& Lightmapper::get() {
    static Lightmapper instance;
    return instance;
}

void Lightmapper::clear() {
    triangles.clear();
    bvh.clear();
    triangle_order.clear();
    actor_lightmaps.clear();
    atlas_pixels.clear();
    atlas_width = atlas_height = 0;
    probes.clear();
    probe_count_x = probe_count_y = probe_count_z = 0;
    if (atlas_texture != 0) {
        glDeleteTextures(1, &atlas_texture);
        atlas_texture = 0;
    }
    atlas_dirty = false;
}

// --- Acceleration structure ------------------------------------------------

void Lightmapper::build_bvh() {
    bvh.clear();
    triangle_order.resize(triangles.size());
    for (size_t i = 0; i < triangles.size(); ++i) triangle_order[i] = static_cast<int>(i);
    if (triangles.empty()) return;

    // Recursive median split. Written as an explicit lambda rather than a member so
    // the recursion can capture the node array it is growing.
    std::function<int(int, int)> build = [&](int first, int count) -> int {
        const int node_index = static_cast<int>(bvh.size());
        bvh.push_back(BVHNode{});

        Vector3 bounds_min = { 1e30f, 1e30f, 1e30f };
        Vector3 bounds_max = { -1e30f, -1e30f, -1e30f };
        for (int i = first; i < first + count; ++i) {
            const Triangle& tri = triangles[triangle_order[i]];
            for (int corner = 0; corner < 3; ++corner) {
                bounds_min.x = std::min(bounds_min.x, tri.position[corner].x);
                bounds_min.y = std::min(bounds_min.y, tri.position[corner].y);
                bounds_min.z = std::min(bounds_min.z, tri.position[corner].z);
                bounds_max.x = std::max(bounds_max.x, tri.position[corner].x);
                bounds_max.y = std::max(bounds_max.y, tri.position[corner].y);
                bounds_max.z = std::max(bounds_max.z, tri.position[corner].z);
            }
        }

        bvh[node_index].bounds_min = bounds_min;
        bvh[node_index].bounds_max = bounds_max;

        // Eight triangles is where walking the list beats another level of tree.
        if (count <= 8) {
            bvh[node_index].left = -1;
            bvh[node_index].first = first;
            bvh[node_index].count = count;
            return node_index;
        }

        // Split along whichever axis the box is longest in, which is the cheap
        // approximation of the split that would separate the triangles best.
        const Vector3 extent = bounds_max - bounds_min;
        const int axis = (extent.x > extent.y)
            ? ((extent.x > extent.z) ? 0 : 2)
            : ((extent.y > extent.z) ? 1 : 2);

        const int middle = first + count / 2;
        std::nth_element(triangle_order.begin() + first,
                         triangle_order.begin() + middle,
                         triangle_order.begin() + first + count,
                         [&](int a, int b) {
                             const Triangle& ta = triangles[a];
                             const Triangle& tb = triangles[b];
                             const float ca = (axis == 0) ? (ta.position[0].x + ta.position[1].x + ta.position[2].x)
                                            : (axis == 1) ? (ta.position[0].y + ta.position[1].y + ta.position[2].y)
                                                          : (ta.position[0].z + ta.position[1].z + ta.position[2].z);
                             const float cb = (axis == 0) ? (tb.position[0].x + tb.position[1].x + tb.position[2].x)
                                            : (axis == 1) ? (tb.position[0].y + tb.position[1].y + tb.position[2].y)
                                                          : (tb.position[0].z + tb.position[1].z + tb.position[2].z);
                             return ca < cb;
                         });

        const int left_child = build(first, middle - first);
        const int right_child = build(middle, first + count - middle);
        // The children are always adjacent in the array, so one index locates both.
        bvh[node_index].left = left_child;
        bvh[node_index].count = 0;
        bvh[node_index].first = right_child;
        return node_index;
    };

    build(0, static_cast<int>(triangles.size()));
}

int Lightmapper::trace(const Vector3& origin, const Vector3& direction, float max_distance,
                       float& out_distance) const {
    if (bvh.empty()) return -1;

    const Vector3 inv_direction = {
        1.0f / ((std::abs(direction.x) > 1e-12f) ? direction.x : 1e-12f),
        1.0f / ((std::abs(direction.y) > 1e-12f) ? direction.y : 1e-12f),
        1.0f / ((std::abs(direction.z) > 1e-12f) ? direction.z : 1e-12f)
    };

    int best_triangle = -1;
    float best_distance = max_distance;

    // Explicit stack rather than recursion: traversal is the hot loop of the whole
    // bake and a call per node costs more than the test does.
    int stack[64];
    int stack_size = 0;
    stack[stack_size++] = 0;

    while (stack_size > 0) {
        const BVHNode& node = bvh[stack[--stack_size]];
        if (!ray_box(origin, inv_direction, node.bounds_min, node.bounds_max, best_distance)) continue;

        if (node.left < 0) {
            for (int i = node.first; i < node.first + node.count; ++i) {
                const Triangle& tri = triangles[triangle_order[i]];
                const float distance = ray_triangle(origin, direction,
                                                    tri.position[0], tri.position[1], tri.position[2]);
                if (distance > 1e-4f && distance < best_distance) {
                    best_distance = distance;
                    best_triangle = triangle_order[i];
                }
            }
        } else if (stack_size + 2 <= 64) {
            stack[stack_size++] = node.left;
            stack[stack_size++] = node.first; // right child
        }
    }

    out_distance = best_distance;
    return best_triangle;
}

bool Lightmapper::occluded(const Vector3& origin, const Vector3& direction, float max_distance) const {
    float distance = 0.0f;
    return trace(origin, direction, max_distance, distance) >= 0;
}

// --- Gathering -------------------------------------------------------------

Vector3 Lightmapper::gather(const Vector3& position, const Vector3& normal, int ray_count,
                            unsigned int& random_state) const {
    Vector3 result = { 0.0f, 0.0f, 0.0f };

    // Offset along the normal, or the very first ray hits the surface it started on.
    const Vector3 origin = position + normal * 0.002f;

    // --- Direct sun --------------------------------------------------------
    const Vector3 to_sun = (settings.sun_direction * -1.0f).normalized();
    const float sun_facing = Vector3::dot(normal, to_sun);
    if (sun_facing > 0.0f && !occluded(origin, to_sun, 1.0e5f)) {
        result += settings.sun_color * (settings.sun_intensity * sun_facing);
    }

    // --- Sky and one bounce ------------------------------------------------
    const Vector3 sky = settings.sky_color * settings.sky_intensity;
    Vector3 indirect = { 0.0f, 0.0f, 0.0f };
    const int rays = std::max(1, ray_count);

    for (int i = 0; i < rays; ++i) {
        const Vector3 direction = cosine_hemisphere(normal, random_state);
        float distance = 0.0f;
        const int hit = trace(origin, direction, 1.0e5f, distance);

        if (hit < 0) {
            // Escaped: this direction sees open sky.
            indirect += sky;
            continue;
        }

        // Hit a surface. Its colour, lit by the sky and by the sun if it can see it,
        // is the single bounce - which is where colour bleed comes from.
        const Triangle& tri = triangles[hit];
        const Vector3 hit_position = origin + direction * distance;
        const Vector3 hit_origin = hit_position + tri.normal * 0.002f;

        Vector3 bounce = sky * 0.5f;
        const float hit_sun_facing = Vector3::dot(tri.normal, to_sun);
        if (hit_sun_facing > 0.0f && !occluded(hit_origin, to_sun, 1.0e5f)) {
            bounce += settings.sun_color * (settings.sun_intensity * hit_sun_facing);
        }
        indirect += tri.albedo * bounce;
    }

    // Cosine-weighted sampling already carries the N-dot-L factor, so the estimator
    // is a plain average.
    result += indirect / static_cast<float>(rays);
    return result;
}

// --- Bake ------------------------------------------------------------------

bool Lightmapper::bake(const std::vector<std::shared_ptr<Actor>>& actors, const BakeSettings& bake_settings,
                       const std::function<void(const char*, float)>& progress, std::string& out_report) {
    clear();
    settings = bake_settings;
    settings.atlas_size = std::max(64, std::min(4096, settings.atlas_size));

    auto report_progress = [&](const char* label, float fraction) {
        if (progress) progress(label, fraction);
    };
    report_progress("Collecting static geometry", 0.0f);

    // --- Collect -----------------------------------------------------------
    // One entry per actor that will receive a lightmap, alongside the shared
    // triangle soup every ray is traced against.
    struct BakeTarget {
        Actor* actor = nullptr;
        StaticMeshComponent* mesh = nullptr;
        const std::vector<Vertex>* vertices = nullptr;
        const std::vector<unsigned int>* indices = nullptr;
        Matrix4x4 model;
        Vector3 albedo;
    };
    std::vector<BakeTarget> targets;

    for (const auto& actor : actors) {
        if (!actor || !actor->is_static) continue;
        for (const auto& comp : actor->get_components()) {
            auto* mesh = dynamic_cast<StaticMeshComponent*>(comp.get());
            if (!mesh) continue;

            const std::vector<Vertex>* vertices = nullptr;
            const std::vector<unsigned int>* indices = nullptr;
            if (auto resource = mesh->get_mesh_resource()) {
                const ResourceState state = resource->get_state();
                if (state != ResourceState::LoadedCPU && state != ResourceState::LoadedGPU) continue;
                vertices = &resource->get_cpu_vertices();
                indices = &resource->get_cpu_indices();
            } else {
                vertices = &mesh->get_vertices();
                indices = &mesh->get_indices();
            }
            if (!vertices || !indices || vertices->empty() || indices->size() < 3) continue;

            BakeTarget target;
            target.actor = actor.get();
            target.mesh = mesh;
            target.vertices = vertices;
            target.indices = indices;
            target.model = mesh->transform.get_matrix();
            target.albedo = actor->actor_color;
            targets.push_back(target);
            break; // one lightmapped mesh per actor
        }
    }

    if (targets.empty()) {
        out_report = "Nothing to bake. Mark the actors that should receive baked light as static.";
        last_report = out_report;
        return false;
    }

    for (const BakeTarget& target : targets) {
        for (size_t i = 0; i + 2 < target.indices->size(); i += 3) {
            const unsigned int i0 = (*target.indices)[i];
            const unsigned int i1 = (*target.indices)[i + 1];
            const unsigned int i2 = (*target.indices)[i + 2];
            if (i0 >= target.vertices->size() || i1 >= target.vertices->size() ||
                i2 >= target.vertices->size()) continue;

            Triangle tri;
            tri.position[0] = target.model * (*target.vertices)[i0].position;
            tri.position[1] = target.model * (*target.vertices)[i1].position;
            tri.position[2] = target.model * (*target.vertices)[i2].position;
            tri.normal = Vector3::cross(tri.position[1] - tri.position[0],
                                        tri.position[2] - tri.position[0]).normalized();
            tri.albedo = target.albedo;
            triangles.push_back(tri);
        }
    }

    if (triangles.empty()) {
        out_report = "Static actors were found but none had usable geometry.";
        last_report = out_report;
        return false;
    }

    report_progress("Building the ray acceleration structure", 0.05f);
    build_bvh();

    // --- Atlas layout ------------------------------------------------------
    // Every triangle is its own chart. Sharing charts between adjacent triangles
    // packs better, but needs a real UV unwrapper; per-triangle charts always work,
    // for any mesh, with no assumptions about its topology - at the cost of a gutter
    // around each one.
    report_progress("Laying out the atlas", 0.1f);

    atlas_width = atlas_height = settings.atlas_size;
    atlas_pixels.assign(static_cast<size_t>(atlas_width) * atlas_height * 3, 0.0f);
    // Which texels were written, so the dilation pass knows what to fill from.
    std::vector<uint8_t> covered(static_cast<size_t>(atlas_width) * atlas_height, 0);

    struct Chart {
        int triangle_index;   // index into `triangles`
        int target_index;
        int x = 0, y = 0, size = 0;
        // Which of the mesh's vertices this chart's three corners belong to.
        unsigned int vertex[3] = { 0, 0, 0 };
    };
    std::vector<Chart> charts;

    int global_triangle = 0;
    for (size_t target_index = 0; target_index < targets.size(); ++target_index) {
        const BakeTarget& target = targets[target_index];
        for (size_t i = 0; i + 2 < target.indices->size(); i += 3) {
            const unsigned int i0 = (*target.indices)[i];
            const unsigned int i1 = (*target.indices)[i + 1];
            const unsigned int i2 = (*target.indices)[i + 2];
            if (i0 >= target.vertices->size() || i1 >= target.vertices->size() ||
                i2 >= target.vertices->size()) continue;

            const Triangle& tri = triangles[global_triangle];
            // Chart size from the triangle's world-space extent, so a large wall gets
            // more texels than a doorknob rather than every triangle getting the same.
            const float edge_a = (tri.position[1] - tri.position[0]).length();
            const float edge_b = (tri.position[2] - tri.position[0]).length();
            const float longest = std::max(edge_a, edge_b);
            int size = static_cast<int>(std::ceil(longest * settings.texels_per_unit));
            size = std::max(2, std::min(64, size));

            Chart chart;
            chart.triangle_index = global_triangle;
            chart.target_index = static_cast<int>(target_index);
            chart.size = size;
            chart.vertex[0] = i0;
            chart.vertex[1] = i1;
            chart.vertex[2] = i2;
            charts.push_back(chart);
            ++global_triangle;
        }
    }

    // Shelf packing, largest first. Not optimal, but it is one pass and it never
    // overlaps - and an overlapping lightmap is worse than a slightly wasteful one.
    std::sort(charts.begin(), charts.end(),
              [](const Chart& a, const Chart& b) { return a.size > b.size; });

    int pen_x = kChartPadding;
    int pen_y = kChartPadding;
    int shelf_height = 0;
    int packed = 0;
    for (Chart& chart : charts) {
        const int footprint = chart.size + kChartPadding * 2;
        if (pen_x + footprint > atlas_width) {
            pen_x = kChartPadding;
            pen_y += shelf_height;
            shelf_height = 0;
        }
        if (pen_y + footprint > atlas_height) {
            chart.size = 0; // did not fit
            continue;
        }
        chart.x = pen_x;
        chart.y = pen_y;
        pen_x += footprint;
        shelf_height = std::max(shelf_height, footprint);
        ++packed;
    }

    const int overflow = static_cast<int>(charts.size()) - packed;

    // --- Per-actor UVs -----------------------------------------------------
    actor_lightmaps.clear();
    actor_lightmaps.resize(targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        actor_lightmaps[i].actor_name = targets[i].actor->get_name();
        actor_lightmaps[i].uvs.assign(targets[i].vertices->size(), Vector2(0.0f, 0.0f));
    }

    // --- Rasterise and gather ----------------------------------------------
    unsigned int random_state = 0x9E3779B9u;
    const float inv_atlas = 1.0f / static_cast<float>(atlas_width);

    for (size_t chart_index = 0; chart_index < charts.size(); ++chart_index) {
        const Chart& chart = charts[chart_index];
        if (chart.size <= 0) continue;

        if ((chart_index & 63) == 0) {
            std::ostringstream label;
            label << "Baking texels (" << chart_index << " / " << charts.size() << ")";
            report_progress(label.str().c_str(), 0.1f + 0.75f * static_cast<float>(chart_index) / charts.size());
        }

        const Triangle& tri = triangles[chart.triangle_index];

        // The chart's three corners in atlas space. The triangle is mapped into a
        // right triangle filling the chart, which wastes half the square but needs
        // no per-triangle parameterisation.
        const Vector2 corner_uv[3] = {
            Vector2(static_cast<float>(chart.x), static_cast<float>(chart.y)),
            Vector2(static_cast<float>(chart.x + chart.size), static_cast<float>(chart.y)),
            Vector2(static_cast<float>(chart.x), static_cast<float>(chart.y + chart.size))
        };

        // Written back so the mesh knows where to sample. A vertex shared between
        // triangles ends up with the last chart's coordinate, which is exactly why
        // charts need their own texels and a gutter - the neighbours disagree.
        ActorLightmap& lightmap = actor_lightmaps[chart.target_index];
        for (int corner = 0; corner < 3; ++corner) {
            const unsigned int vertex = chart.vertex[corner];
            if (vertex < lightmap.uvs.size()) {
                lightmap.uvs[vertex] = Vector2((corner_uv[corner].x + 0.5f) * inv_atlas,
                                               (corner_uv[corner].y + 0.5f) * inv_atlas);
            }
        }

        // Walk the chart's texels, map each back onto the triangle, and gather.
        for (int local_y = 0; local_y <= chart.size; ++local_y) {
            for (int local_x = 0; local_x <= chart.size; ++local_x) {
                // Barycentric weights of this texel within the right triangle.
                const float u = static_cast<float>(local_x) / chart.size;
                const float v = static_cast<float>(local_y) / chart.size;
                if (u + v > 1.0f) continue; // outside the mapped half

                const Vector3 position = tri.position[0] +
                                         (tri.position[1] - tri.position[0]) * u +
                                         (tri.position[2] - tri.position[0]) * v;

                const int atlas_x = chart.x + local_x;
                const int atlas_y = chart.y + local_y;
                if (atlas_x < 0 || atlas_y < 0 || atlas_x >= atlas_width || atlas_y >= atlas_height) continue;

                const Vector3 light = gather(position, tri.normal, settings.rays_per_texel, random_state);

                const size_t texel = (static_cast<size_t>(atlas_y) * atlas_width + atlas_x);
                atlas_pixels[texel * 3 + 0] = light.x;
                atlas_pixels[texel * 3 + 1] = light.y;
                atlas_pixels[texel * 3 + 2] = light.z;
                covered[texel] = 1;
            }
        }
    }

    // --- Dilate ------------------------------------------------------------
    // Bleed written texels outward into the gutters. Without it, bilinear filtering
    // at a chart's edge mixes in the black of an unwritten texel and every chart
    // gets a dark outline.
    report_progress("Filling gutters", 0.86f);
    for (int pass = 0; pass < kChartPadding + 1; ++pass) {
        std::vector<uint8_t> next_covered = covered;
        for (int y = 0; y < atlas_height; ++y) {
            for (int x = 0; x < atlas_width; ++x) {
                const size_t texel = static_cast<size_t>(y) * atlas_width + x;
                if (covered[texel]) continue;

                Vector3 sum = { 0.0f, 0.0f, 0.0f };
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= atlas_width || ny >= atlas_height) continue;
                        const size_t neighbour = static_cast<size_t>(ny) * atlas_width + nx;
                        if (!covered[neighbour]) continue;
                        sum.x += atlas_pixels[neighbour * 3 + 0];
                        sum.y += atlas_pixels[neighbour * 3 + 1];
                        sum.z += atlas_pixels[neighbour * 3 + 2];
                        ++count;
                    }
                }
                if (count == 0) continue;
                atlas_pixels[texel * 3 + 0] = sum.x / count;
                atlas_pixels[texel * 3 + 1] = sum.y / count;
                atlas_pixels[texel * 3 + 2] = sum.z / count;
                next_covered[texel] = 1;
            }
        }
        covered.swap(next_covered);
    }

    // --- Probes ------------------------------------------------------------
    if (settings.bake_probes) {
        report_progress("Baking light probes", 0.9f);

        Vector3 scene_min = { 1e30f, 1e30f, 1e30f };
        Vector3 scene_max = { -1e30f, -1e30f, -1e30f };
        for (const Triangle& tri : triangles) {
            for (int corner = 0; corner < 3; ++corner) {
                scene_min.x = std::min(scene_min.x, tri.position[corner].x);
                scene_min.y = std::min(scene_min.y, tri.position[corner].y);
                scene_min.z = std::min(scene_min.z, tri.position[corner].z);
                scene_max.x = std::max(scene_max.x, tri.position[corner].x);
                scene_max.y = std::max(scene_max.y, tri.position[corner].y);
                scene_max.z = std::max(scene_max.z, tri.position[corner].z);
            }
        }

        probe_spacing = std::max(0.5f, settings.probe_spacing);
        // One probe past each edge, so an object standing at the boundary still has
        // eight probes to interpolate between rather than falling off the grid.
        probe_origin = { static_cast<double>(scene_min.x) - probe_spacing,
                         static_cast<double>(scene_min.y) - probe_spacing,
                         static_cast<double>(scene_min.z) - probe_spacing };

        probe_count_x = std::max(2, static_cast<int>(std::ceil((scene_max.x - scene_min.x) / probe_spacing)) + 3);
        probe_count_y = std::max(2, static_cast<int>(std::ceil((scene_max.y - scene_min.y) / probe_spacing)) + 3);
        probe_count_z = std::max(2, static_cast<int>(std::ceil((scene_max.z - scene_min.z) / probe_spacing)) + 3);

        // A ceiling on the grid: spacing is a number a person types, and a small one
        // in a large level asks for millions of probes at 64 rays each.
        const long long total = static_cast<long long>(probe_count_x) * probe_count_y * probe_count_z;
        constexpr long long kMaxProbes = 60000;
        if (total > kMaxProbes) {
            std::ostringstream note;
            note << "Probe grid would be " << total << " probes; skipped. Increase the spacing.";
            last_report = note.str();
            probe_count_x = probe_count_y = probe_count_z = 0;
        } else {
            probes.assign(static_cast<size_t>(total), AmbientCube{});
            const Vector3 axes[6] = {
                { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
                { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f }
            };

            size_t index = 0;
            for (int z = 0; z < probe_count_z; ++z) {
                for (int y = 0; y < probe_count_y; ++y) {
                    for (int x = 0; x < probe_count_x; ++x, ++index) {
                        const Vector3 position = {
                            static_cast<float>(probe_origin.x) + x * probe_spacing,
                            static_cast<float>(probe_origin.y) + y * probe_spacing,
                            static_cast<float>(probe_origin.z) + z * probe_spacing
                        };
                        // Each face of the cube is gathered as if it were a surface
                        // facing that way, which is exactly what the cube is asked
                        // for at lookup time.
                        for (int face = 0; face < 6; ++face) {
                            probes[index].axis[face] =
                                gather(position, axes[face], std::max(8, settings.rays_per_texel / 4),
                                       random_state);
                        }
                    }
                }
            }
        }
    }

    atlas_dirty = true;
    report_progress("Done", 1.0f);

    std::ostringstream message;
    message << packed << " charts in a " << atlas_width << "px atlas across "
            << targets.size() << " actor(s), " << triangles.size() << " triangles";
    if (!probes.empty()) {
        message << ", " << probes.size() << " probes (" << probe_count_x << "x"
                << probe_count_y << "x" << probe_count_z << ")";
    }
    if (overflow > 0) {
        message << ". " << overflow << " charts did not fit - raise the atlas size or "
                   "lower the texel density.";
    }
    out_report = message.str();
    last_report = out_report;

    // Freed once the bake is done: the tree is only needed while tracing, and it is
    // the largest thing here.
    triangles.clear();
    bvh.clear();
    triangle_order.clear();
    return true;
}

// --- Lookup ----------------------------------------------------------------

void Lightmapper::apply_to_actors(const std::vector<std::shared_ptr<Actor>>& actors) const {
    for (const auto& actor : actors) {
        if (!actor) continue;
        auto* mesh = actor->get_component<StaticMeshComponent>();
        if (!mesh) continue;

        const std::vector<Vector2>* uvs = get_actor_uvs(actor->get_name());
        // An actor with no entry is not lightmapped and must be cleared, or a
        // re-bake that dropped it would leave it sampling a stale region of the
        // atlas that now belongs to something else.
        if (uvs && !uvs->empty()) mesh->set_lightmap_uvs(*uvs);
        else mesh->clear_lightmap();
    }
}

const std::vector<Vector2>* Lightmapper::get_actor_uvs(const std::string& actor_name) const {
    for (const ActorLightmap& lightmap : actor_lightmaps) {
        if (lightmap.actor_name == actor_name) return &lightmap.uvs;
    }
    return nullptr;
}

Lightmapper::AmbientCube Lightmapper::sample_probes(const DVector3& position) const {
    AmbientCube result;
    if (probes.empty() || probe_count_x <= 0) {
        // Nothing baked contributes nothing. Returning sky light here instead would
        // add an ambient term to every surface of every scene the moment this system
        // existed - on top of the IBL and analytic ambient the lighting pass already
        // applies - silently brightening content authored before any of this. Baked
        // GI is strictly additive on top of a bake the user asked for.
        return result;
    }

    const double fx = (position.x - probe_origin.x) / probe_spacing;
    const double fy = (position.y - probe_origin.y) / probe_spacing;
    const double fz = (position.z - probe_origin.z) / probe_spacing;

    const int x0 = std::max(0, std::min(probe_count_x - 1, static_cast<int>(std::floor(fx))));
    const int y0 = std::max(0, std::min(probe_count_y - 1, static_cast<int>(std::floor(fy))));
    const int z0 = std::max(0, std::min(probe_count_z - 1, static_cast<int>(std::floor(fz))));
    const int x1 = std::min(probe_count_x - 1, x0 + 1);
    const int y1 = std::min(probe_count_y - 1, y0 + 1);
    const int z1 = std::min(probe_count_z - 1, z0 + 1);

    const float tx = static_cast<float>(std::max(0.0, std::min(1.0, fx - x0)));
    const float ty = static_cast<float>(std::max(0.0, std::min(1.0, fy - y0)));
    const float tz = static_cast<float>(std::max(0.0, std::min(1.0, fz - z0)));

    auto probe_at = [&](int x, int y, int z) -> const AmbientCube& {
        const size_t index = (static_cast<size_t>(z) * probe_count_y + y) * probe_count_x + x;
        return probes[index];
    };

    // Trilinear across the eight surrounding probes, per cube face.
    for (int face = 0; face < 6; ++face) {
        auto blend = [&](const Vector3& a, const Vector3& b, float t) {
            return Vector3{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
        };
        const Vector3 c00 = blend(probe_at(x0, y0, z0).axis[face], probe_at(x1, y0, z0).axis[face], tx);
        const Vector3 c10 = blend(probe_at(x0, y1, z0).axis[face], probe_at(x1, y1, z0).axis[face], tx);
        const Vector3 c01 = blend(probe_at(x0, y0, z1).axis[face], probe_at(x1, y0, z1).axis[face], tx);
        const Vector3 c11 = blend(probe_at(x0, y1, z1).axis[face], probe_at(x1, y1, z1).axis[face], tx);
        const Vector3 c0 = blend(c00, c10, ty);
        const Vector3 c1 = blend(c01, c11, ty);
        result.axis[face] = blend(c0, c1, tz);
    }
    return result;
}

unsigned int Lightmapper::get_atlas_texture() {
    if (atlas_pixels.empty() || atlas_width <= 0) return 0;

    if (atlas_texture == 0) {
        glGenTextures(1, &atlas_texture);
        glBindTexture(GL_TEXTURE_2D, atlas_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        atlas_dirty = true;
    }

    if (atlas_dirty) {
        glBindTexture(GL_TEXTURE_2D, atlas_texture);
        // Floating point, because bounce light spans orders of magnitude and eight
        // bits bands every gradient it is asked to hold.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, atlas_width, atlas_height, 0,
                     GL_RGB, GL_FLOAT, atlas_pixels.data());
        atlas_dirty = false;
    }
    return atlas_texture;
}

// --- Persistence -----------------------------------------------------------

bool Lightmapper::save(const std::string& filepath) const {
    if (!is_baked()) return false;

    std::ofstream file(filepath, std::ios::binary);
    if (!file) {
        std::cerr << "[Lightmap] Could not write " << filepath << std::endl;
        return false;
    }

    const uint32_t magic = kLightmapMagic;
    const uint32_t version = kLightmapVersion;
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&atlas_width), sizeof(atlas_width));
    file.write(reinterpret_cast<const char*>(&atlas_height), sizeof(atlas_height));
    file.write(reinterpret_cast<const char*>(atlas_pixels.data()),
               atlas_pixels.size() * sizeof(float));

    const uint32_t actor_count = static_cast<uint32_t>(actor_lightmaps.size());
    file.write(reinterpret_cast<const char*>(&actor_count), sizeof(actor_count));
    for (const ActorLightmap& lightmap : actor_lightmaps) {
        const uint32_t name_length = static_cast<uint32_t>(lightmap.actor_name.size());
        file.write(reinterpret_cast<const char*>(&name_length), sizeof(name_length));
        file.write(lightmap.actor_name.data(), name_length);
        const uint32_t uv_count = static_cast<uint32_t>(lightmap.uvs.size());
        file.write(reinterpret_cast<const char*>(&uv_count), sizeof(uv_count));
        file.write(reinterpret_cast<const char*>(lightmap.uvs.data()), uv_count * sizeof(Vector2));
    }

    file.write(reinterpret_cast<const char*>(&probe_origin), sizeof(probe_origin));
    file.write(reinterpret_cast<const char*>(&probe_spacing), sizeof(probe_spacing));
    file.write(reinterpret_cast<const char*>(&probe_count_x), sizeof(probe_count_x));
    file.write(reinterpret_cast<const char*>(&probe_count_y), sizeof(probe_count_y));
    file.write(reinterpret_cast<const char*>(&probe_count_z), sizeof(probe_count_z));
    const uint32_t probe_count = static_cast<uint32_t>(probes.size());
    file.write(reinterpret_cast<const char*>(&probe_count), sizeof(probe_count));
    if (probe_count > 0) {
        file.write(reinterpret_cast<const char*>(probes.data()), probe_count * sizeof(AmbientCube));
    }
    return static_cast<bool>(file);
}

bool Lightmapper::load(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) return false;

    uint32_t magic = 0, version = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!file || magic != kLightmapMagic || version != kLightmapVersion) {
        std::cerr << "[Lightmap] " << filepath << " is not a lightmap this build understands."
                  << std::endl;
        return false;
    }

    clear();

    int width = 0, height = 0;
    file.read(reinterpret_cast<char*>(&width), sizeof(width));
    file.read(reinterpret_cast<char*>(&height), sizeof(height));
    if (!file || width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        std::cerr << "[Lightmap] " << filepath << " has implausible dimensions." << std::endl;
        return false;
    }

    atlas_width = width;
    atlas_height = height;
    atlas_pixels.assign(static_cast<size_t>(width) * height * 3, 0.0f);
    file.read(reinterpret_cast<char*>(atlas_pixels.data()), atlas_pixels.size() * sizeof(float));

    uint32_t actor_count = 0;
    file.read(reinterpret_cast<char*>(&actor_count), sizeof(actor_count));
    for (uint32_t i = 0; i < actor_count && file; ++i) {
        ActorLightmap lightmap;
        uint32_t name_length = 0;
        file.read(reinterpret_cast<char*>(&name_length), sizeof(name_length));
        if (name_length > 4096) break;
        lightmap.actor_name.assign(name_length, '\0');
        if (name_length > 0) file.read(lightmap.actor_name.data(), name_length);

        uint32_t uv_count = 0;
        file.read(reinterpret_cast<char*>(&uv_count), sizeof(uv_count));
        if (uv_count > 50'000'000u) break;
        lightmap.uvs.assign(uv_count, Vector2(0.0f, 0.0f));
        if (uv_count > 0) {
            file.read(reinterpret_cast<char*>(lightmap.uvs.data()), uv_count * sizeof(Vector2));
        }
        actor_lightmaps.push_back(std::move(lightmap));
    }

    file.read(reinterpret_cast<char*>(&probe_origin), sizeof(probe_origin));
    file.read(reinterpret_cast<char*>(&probe_spacing), sizeof(probe_spacing));
    file.read(reinterpret_cast<char*>(&probe_count_x), sizeof(probe_count_x));
    file.read(reinterpret_cast<char*>(&probe_count_y), sizeof(probe_count_y));
    file.read(reinterpret_cast<char*>(&probe_count_z), sizeof(probe_count_z));

    uint32_t probe_count = 0;
    file.read(reinterpret_cast<char*>(&probe_count), sizeof(probe_count));
    if (file && probe_count > 0 && probe_count < 5'000'000u) {
        probes.assign(probe_count, AmbientCube{});
        file.read(reinterpret_cast<char*>(probes.data()), probe_count * sizeof(AmbientCube));
    }

    if (!file) {
        std::cerr << "[Lightmap] " << filepath << " ended early; it is truncated." << std::endl;
        clear();
        return false;
    }

    atlas_dirty = true;
    return true;
}
