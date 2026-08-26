#include "world/terrain_component.hpp"
#include "world/actor.hpp"
#include "core/mesh_resource.hpp"
#include "core/texture_resource.hpp"
#include "core/resource_manager.hpp"
#include "physics/physics_engine.hpp"
#include "renderer/gl_loader.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <iostream>

namespace {

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

// Deterministic hash of three integers to [0, 1). Foliage placement has to be
// reproducible: the same terrain must scatter the same trees every run, and on
// every machine, or a saved scene changes shape when it is reopened.
float hash01(int x, int y, int z) {
    uint32_t h = static_cast<uint32_t>(x) * 0x9E3779B1u;
    h ^= static_cast<uint32_t>(y) * 0x85EBCA77u;
    h ^= static_cast<uint32_t>(z) * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return static_cast<float>(h & 0x00FFFFFFu) / 16777216.0f;
}

int round_to_valid_resolution(int requested) {
    int value = std::max(TerrainComponent::kMinResolution,
                         std::min(TerrainComponent::kMaxResolution, requested));
    // Rounded down to a power of two: Jolt's height field wants a sample count
    // divisible by its block size, and a power of two is what it packs best.
    int power = TerrainComponent::kMinResolution;
    while (power * 2 <= value) power *= 2;
    return power;
}

} // namespace

const char* TerrainComponent::sculpt_tool_name(int tool) {
    switch (tool) {
        case Sculpt_Raise:   return "Raise";
        case Sculpt_Lower:   return "Lower";
        case Sculpt_Smooth:  return "Smooth";
        case Sculpt_Flatten: return "Flatten";
        default:             return "Unknown";
    }
}

TerrainComponent::TerrainComponent(Actor* owner, const std::string& name)
    : SceneComponent(owner, name) {
    resize(resolution, world_size);
    // Terrain is the ground; an actor that has one and nothing else would otherwise
    // have no root and sit at a shared default transform.
    if (owner && !owner->get_root_component()) owner->set_root_component(this);
}

TerrainComponent::~TerrainComponent() {
    destroy_collision();
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (ebo) glDeleteBuffers(1, &ebo);
    if (splat_texture) glDeleteTextures(1, &splat_texture);
}

const Transform& TerrainComponent::placement() const {
    // The component's own transform is the fallback for an ownerless component,
    // which only happens in a partially constructed state.
    return owner ? owner->get_actor_transform() : transform;
}

// --- Shape -----------------------------------------------------------------

float TerrainComponent::sample_to_local(int index) const {
    // Samples span the full extent, centred on the component's origin.
    const float step = world_size / static_cast<float>(resolution - 1);
    return -world_size * 0.5f + index * step;
}

void TerrainComponent::local_to_sample(float local_x, float local_z, float& out_fx, float& out_fz) const {
    const float step = world_size / static_cast<float>(resolution - 1);
    out_fx = (local_x + world_size * 0.5f) / step;
    out_fz = (local_z + world_size * 0.5f) / step;
}

void TerrainComponent::resize(int new_resolution, float new_world_size) {
    const int target = round_to_valid_resolution(new_resolution);
    new_world_size = std::max(1.0f, new_world_size);

    if (!heights.empty() && target == resolution) {
        // Only the extent changed: the samples still describe the same surface, just
        // stretched over a different area.
        world_size = new_world_size;
        rebuild_geometry();
        rebuild_collision();
        foliage_dirty = true;
        return;
    }

    std::vector<float> resampled(static_cast<size_t>(target) * target, 0.0f);
    if (!heights.empty() && resolution > 1) {
        // Resample rather than discard: raising the resolution of a sculpted terrain
        // should refine it, not flatten hours of work.
        for (int z = 0; z < target; ++z) {
            const float v = static_cast<float>(z) / (target - 1);
            for (int x = 0; x < target; ++x) {
                const float u = static_cast<float>(x) / (target - 1);
                const float local_x = -world_size * 0.5f + u * world_size;
                const float local_z = -world_size * 0.5f + v * world_size;
                resampled[static_cast<size_t>(z) * target + x] = sample_height(local_x, local_z);
            }
        }
    }

    std::vector<unsigned char> resampled_splat(static_cast<size_t>(target) * target * 4, 0);
    std::vector<unsigned char> resampled_foliage(static_cast<size_t>(target) * target, 0);
    if (!splat.empty() && resolution > 1) {
        for (int z = 0; z < target; ++z) {
            const int source_z = std::min(resolution - 1, z * (resolution - 1) / std::max(1, target - 1));
            for (int x = 0; x < target; ++x) {
                const int source_x = std::min(resolution - 1, x * (resolution - 1) / std::max(1, target - 1));
                const size_t source = (static_cast<size_t>(source_z) * resolution + source_x);
                const size_t destination = (static_cast<size_t>(z) * target + x);
                for (int channel = 0; channel < 4; ++channel) {
                    resampled_splat[destination * 4 + channel] = splat[source * 4 + channel];
                }
                resampled_foliage[destination] = foliage_coverage[source];
            }
        }
    } else {
        // Fresh terrain: fully covered by the first layer, so it is not invisible.
        for (size_t i = 0; i < resampled_splat.size(); i += 4) resampled_splat[i] = 255;
    }

    resolution = target;
    world_size = new_world_size;
    heights = std::move(resampled);
    splat = std::move(resampled_splat);
    foliage_coverage = std::move(resampled_foliage);

    rebuild_geometry();
    rebuild_collision();
    foliage_dirty = true;
}

float TerrainComponent::sample_height(float local_x, float local_z) const {
    if (heights.empty()) return 0.0f;

    float fx, fz;
    local_to_sample(local_x, local_z, fx, fz);
    fx = std::max(0.0f, std::min(static_cast<float>(resolution - 1), fx));
    fz = std::max(0.0f, std::min(static_cast<float>(resolution - 1), fz));

    const int x0 = static_cast<int>(fx);
    const int z0 = static_cast<int>(fz);
    const int x1 = std::min(resolution - 1, x0 + 1);
    const int z1 = std::min(resolution - 1, z0 + 1);
    const float tx = fx - x0;
    const float tz = fz - z0;

    const float h00 = heights[index_of(x0, z0)];
    const float h10 = heights[index_of(x1, z0)];
    const float h01 = heights[index_of(x0, z1)];
    const float h11 = heights[index_of(x1, z1)];

    const float top = h00 + (h10 - h00) * tx;
    const float bottom = h01 + (h11 - h01) * tx;
    return top + (bottom - top) * tz;
}

Vector3 TerrainComponent::sample_normal(float local_x, float local_z) const {
    const float step = world_size / static_cast<float>(resolution - 1);
    // Central differences: the surface gradient in x and z, turned into a normal by
    // crossing the two tangents. Cheaper and smoother than reconstructing from the
    // triangles, and continuous across cell boundaries.
    const float hl = sample_height(local_x - step, local_z);
    const float hr = sample_height(local_x + step, local_z);
    const float hd = sample_height(local_x, local_z - step);
    const float hu = sample_height(local_x, local_z + step);
    return Vector3{ hl - hr, 2.0f * step, hd - hu }.normalized();
}

bool TerrainComponent::raycast(const DVector3& origin, const Vector3& direction, float max_distance,
                               DVector3& out_hit) const {
    if (heights.empty()) return false;

    // Everything happens in the terrain's local space, so the surface is an
    // axis-aligned height field and the march is a straight walk.
    const Transform& t = placement();
    const DVector3 local_origin = origin - t.position;
    const Vector3 local_dir = direction.normalized();
    if (std::abs(local_dir.x) < 1e-9f && std::abs(local_dir.y) < 1e-9f && std::abs(local_dir.z) < 1e-9f) {
        return false;
    }

    const float step = std::max(0.05f, world_size / static_cast<float>(resolution - 1));
    float travelled = 0.0f;
    float previous_delta = static_cast<float>(local_origin.y) - sample_height(
        static_cast<float>(local_origin.x), static_cast<float>(local_origin.z));

    while (travelled < max_distance) {
        travelled += step;
        const float x = static_cast<float>(local_origin.x) + local_dir.x * travelled;
        const float y = static_cast<float>(local_origin.y) + local_dir.y * travelled;
        const float z = static_cast<float>(local_origin.z) + local_dir.z * travelled;

        // Outside the footprint there is no surface to hit, but the ray may still
        // cross back in, so keep marching rather than giving up.
        const float half = world_size * 0.5f;
        if (x < -half || x > half || z < -half || z > half) {
            previous_delta = 1e30f;
            continue;
        }

        const float delta = y - sample_height(x, z);
        if (previous_delta > 0.0f && delta <= 0.0f) {
            // Crossed the surface between the last step and this one. One linear
            // refinement is plenty at this step size, and beats bisecting for the
            // handful of pixels of accuracy it would add.
            const float span = previous_delta - delta;
            const float fraction = (span > 1e-6f) ? (previous_delta / span) : 0.0f;
            const float hit_distance = travelled - step * (1.0f - fraction);
            // Back into world space: the march ran in the terrain's local frame, so
            // the component's own position has to be added back on.
            out_hit = t.position + DVector3{
                local_origin.x + static_cast<double>(local_dir.x) * hit_distance,
                local_origin.y + static_cast<double>(local_dir.y) * hit_distance,
                local_origin.z + static_cast<double>(local_dir.z) * hit_distance
            };
            return true;
        }
        previous_delta = delta;
    }
    return false;
}

// --- Sculpting -------------------------------------------------------------

void TerrainComponent::sculpt(int tool, float local_x, float local_z, float radius, float strength,
                              float flatten_height) {
    if (heights.empty() || radius <= 0.0f) return;

    const float step = world_size / static_cast<float>(resolution - 1);
    float fx, fz;
    local_to_sample(local_x, local_z, fx, fz);
    const int reach = static_cast<int>(std::ceil(radius / step));

    const int x0 = std::max(0, static_cast<int>(fx) - reach);
    const int x1 = std::min(resolution - 1, static_cast<int>(fx) + reach + 1);
    const int z0 = std::max(0, static_cast<int>(fz) - reach);
    const int z1 = std::min(resolution - 1, static_cast<int>(fz) + reach + 1);
    if (x0 > x1 || z0 > z1) return;

    // Smooth reads the surface while it writes it, so it needs the original values
    // or the brush drags in the direction the loop happens to run.
    std::vector<float> source;
    if (tool == Sculpt_Smooth) source = heights;

    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = (x - fx) * step;
            const float dz = (z - fz) * step;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (distance > radius) continue;

            // Smoothstep falloff, so a stroke leaves a rounded hill rather than a
            // cone with a hard rim.
            const float t = 1.0f - (distance / radius);
            const float falloff = t * t * (3.0f - 2.0f * t);
            const float amount = strength * falloff;
            float& height = heights[index_of(x, z)];

            switch (tool) {
                case Sculpt_Raise: height += amount; break;
                case Sculpt_Lower: height -= amount; break;
                case Sculpt_Flatten: {
                    const float blend = std::min(1.0f, std::abs(amount));
                    height += (flatten_height - height) * blend;
                    break;
                }
                case Sculpt_Smooth: {
                    float total = 0.0f;
                    int count = 0;
                    for (int nz = std::max(0, z - 1); nz <= std::min(resolution - 1, z + 1); ++nz) {
                        for (int nx = std::max(0, x - 1); nx <= std::min(resolution - 1, x + 1); ++nx) {
                            total += source[index_of(nx, nz)];
                            ++count;
                        }
                    }
                    const float average = (count > 0) ? total / count : height;
                    const float blend = std::min(1.0f, std::abs(amount));
                    height += (average - height) * blend;
                    break;
                }
                default: break;
            }
        }
    }

    mark_height_region_dirty(x0, z0, x1, z1);
    rebuild_collision();
    foliage_dirty = true;
}

void TerrainComponent::paint_layer(int layer, float local_x, float local_z, float radius, float strength) {
    if (splat.empty() || layer < 0 || layer >= kLayerCount || radius <= 0.0f) return;

    const float step = world_size / static_cast<float>(resolution - 1);
    float fx, fz;
    local_to_sample(local_x, local_z, fx, fz);
    const int reach = static_cast<int>(std::ceil(radius / step));

    const int x0 = std::max(0, static_cast<int>(fx) - reach);
    const int x1 = std::min(resolution - 1, static_cast<int>(fx) + reach + 1);
    const int z0 = std::max(0, static_cast<int>(fz) - reach);
    const int z1 = std::min(resolution - 1, static_cast<int>(fz) + reach + 1);

    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = (x - fx) * step;
            const float dz = (z - fz) * step;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (distance > radius) continue;

            const float t = 1.0f - (distance / radius);
            const float falloff = t * t * (3.0f - 2.0f * t);
            const float amount = std::max(0.0f, std::min(1.0f, strength * falloff));

            unsigned char* texel = &splat[(static_cast<size_t>(z) * resolution + x) * 4];
            // Move the target toward full coverage and everything else toward zero
            // by the same fraction. That keeps the four weights summing to 255
            // exactly, which is what lets the shader treat them as a blend.
            for (int channel = 0; channel < kLayerCount; ++channel) {
                const float current = texel[channel] / 255.0f;
                const float target = (channel == layer) ? 1.0f : 0.0f;
                const float blended = current + (target - current) * amount;
                texel[channel] = static_cast<unsigned char>(
                    std::max(0.0f, std::min(255.0f, blended * 255.0f + 0.5f)));
            }

            // Rounding can leave the sum a little off; fold the difference into the
            // layer being painted, which is the one the user is looking at.
            int sum = texel[0] + texel[1] + texel[2] + texel[3];
            const int correction = 255 - sum;
            const int corrected = static_cast<int>(texel[layer]) + correction;
            texel[layer] = static_cast<unsigned char>(std::max(0, std::min(255, corrected)));
        }
    }

    mark_splat_dirty();
}

void TerrainComponent::paint_foliage(float local_x, float local_z, float radius, float strength, bool erase) {
    if (foliage_coverage.empty() || radius <= 0.0f) return;

    const float step = world_size / static_cast<float>(resolution - 1);
    float fx, fz;
    local_to_sample(local_x, local_z, fx, fz);
    const int reach = static_cast<int>(std::ceil(radius / step));

    const int x0 = std::max(0, static_cast<int>(fx) - reach);
    const int x1 = std::min(resolution - 1, static_cast<int>(fx) + reach + 1);
    const int z0 = std::max(0, static_cast<int>(fz) - reach);
    const int z1 = std::min(resolution - 1, static_cast<int>(fz) + reach + 1);

    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = (x - fx) * step;
            const float dz = (z - fz) * step;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (distance > radius) continue;

            const float t = 1.0f - (distance / radius);
            const float falloff = t * t * (3.0f - 2.0f * t);
            const int delta = static_cast<int>(strength * falloff * 255.0f);
            int value = foliage_coverage[index_of(x, z)];
            value += erase ? -delta : delta;
            foliage_coverage[index_of(x, z)] = static_cast<unsigned char>(std::max(0, std::min(255, value)));
        }
    }

    foliage_dirty = true;
}

void TerrainComponent::reset() {
    std::fill(heights.begin(), heights.end(), 0.0f);
    std::fill(foliage_coverage.begin(), foliage_coverage.end(), 0);
    for (size_t i = 0; i < splat.size(); i += 4) {
        splat[i] = 255;
        splat[i + 1] = splat[i + 2] = splat[i + 3] = 0;
    }
    rebuild_geometry();
    mark_splat_dirty();
    rebuild_collision();
    foliage_dirty = true;
}

// --- Geometry --------------------------------------------------------------

void TerrainComponent::get_local_bounds(Vector3& out_min, Vector3& out_max) const {
    float lowest = 0.0f;
    float highest = 0.0f;
    for (float height : heights) {
        lowest = std::min(lowest, height);
        highest = std::max(highest, height);
    }
    const float half = world_size * 0.5f;
    out_min = { -half, lowest, -half };
    out_max = {  half, highest, half };
}

void TerrainComponent::rebuild_geometry() {
    vertices.assign(static_cast<size_t>(resolution) * resolution, Vertex{});
    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            Vertex& v = vertices[index_of(x, z)];
            v.position = { sample_to_local(x), heights[index_of(x, z)], sample_to_local(z) };
            v.color = { 1.0f, 1.0f, 1.0f };
            // Normalised across the whole terrain: the splat map is sampled with
            // this directly, and each layer scales it by its own tiling factor.
            v.uv = { static_cast<float>(x) / (resolution - 1),
                     static_cast<float>(z) / (resolution - 1) };
        }
    }
    recompute_normals_region(0, 0, resolution - 1, resolution - 1);

    indices.clear();
    indices.reserve(static_cast<size_t>(resolution - 1) * (resolution - 1) * 6);
    for (int z = 0; z + 1 < resolution; ++z) {
        for (int x = 0; x + 1 < resolution; ++x) {
            const unsigned int i00 = index_of(x, z);
            const unsigned int i10 = index_of(x + 1, z);
            const unsigned int i01 = index_of(x, z + 1);
            const unsigned int i11 = index_of(x + 1, z + 1);
            // Counter-clockwise seen from above, so the surface normal points up and
            // the navmesh build and back-face culling both agree it is a floor.
            indices.push_back(i00); indices.push_back(i01); indices.push_back(i11);
            indices.push_back(i00); indices.push_back(i11); indices.push_back(i10);
        }
    }
    index_count = static_cast<unsigned int>(indices.size());

    geometry_dirty = true;
    dirty_x0 = 0; dirty_z0 = 0; dirty_x1 = resolution - 1; dirty_z1 = resolution - 1;
}

void TerrainComponent::recompute_normals_region(int x0, int z0, int x1, int z1) {
    // One sample wider than the edited region: a sample's normal depends on its
    // neighbours, so the ring around a stroke changes too.
    x0 = std::max(0, x0 - 1); z0 = std::max(0, z0 - 1);
    x1 = std::min(resolution - 1, x1 + 1); z1 = std::min(resolution - 1, z1 + 1);

    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            vertices[index_of(x, z)].normal = sample_normal(sample_to_local(x), sample_to_local(z));
        }
    }
}

void TerrainComponent::mark_height_region_dirty(int x0, int z0, int x1, int z1) {
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            vertices[index_of(x, z)].position.y = heights[index_of(x, z)];
        }
    }
    recompute_normals_region(x0, z0, x1, z1);

    // Union with whatever was already waiting, so several strokes between two frames
    // are pushed as one upload.
    const int nx0 = std::max(0, x0 - 1), nz0 = std::max(0, z0 - 1);
    const int nx1 = std::min(resolution - 1, x1 + 1), nz1 = std::min(resolution - 1, z1 + 1);
    if (dirty_x1 < dirty_x0) {
        dirty_x0 = nx0; dirty_z0 = nz0; dirty_x1 = nx1; dirty_z1 = nz1;
    } else {
        dirty_x0 = std::min(dirty_x0, nx0); dirty_z0 = std::min(dirty_z0, nz0);
        dirty_x1 = std::max(dirty_x1, nx1); dirty_z1 = std::max(dirty_z1, nz1);
    }
}

void TerrainComponent::mark_splat_dirty() {
    splat_dirty = true;
}

void TerrainComponent::upload_pending() const {
    if (vao == 0) {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);
    }

    glBindVertexArray(vao);

    if (geometry_dirty) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

        // Same layout as every other mesh, which is what lets the shadow depth pass
        // draw the terrain with no special case at all.
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));

        geometry_dirty = false;
        dirty_x1 = -1;
    } else if (dirty_x1 >= dirty_x0) {
        // Rows are contiguous in the buffer, so each edited row is one sub-upload of
        // the span it covers. A brush touches a handful of rows out of hundreds.
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        const size_t span = static_cast<size_t>(dirty_x1 - dirty_x0 + 1) * sizeof(Vertex);
        for (int z = dirty_z0; z <= dirty_z1; ++z) {
            const size_t offset = (static_cast<size_t>(z) * resolution + dirty_x0) * sizeof(Vertex);
            glBufferSubData(GL_ARRAY_BUFFER, offset, span, &vertices[index_of(dirty_x0, z)]);
        }
        dirty_x1 = -1;
    }

    if (splat_dirty) {
        if (splat_texture == 0) {
            glGenTextures(1, &splat_texture);
            glBindTexture(GL_TEXTURE_2D, splat_texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_2D, splat_texture);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, resolution, resolution, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, splat.data());
        splat_dirty = false;
    }

    glBindVertexArray(0);
}

void TerrainComponent::render() const {
    if (indices.empty()) return;
    upload_pending();
    if (vao == 0) return;

    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

void TerrainComponent::bind_material(int splat_unit, int first_layer_unit,
                                     bool out_layer_present[kLayerCount]) const {
    glActiveTexture(GL_TEXTURE0 + splat_unit);
    glBindTexture(GL_TEXTURE_2D, splat_texture);

    for (int layer = 0; layer < kLayerCount; ++layer) {
        // Requested lazily, and re-requested when the path changes, so editing a
        // layer in the inspector takes effect without a scene reload.
        if (!layer_requested[layer] || layer_requested_path[layer] != layer_texture_path[layer]) {
            layer_requested[layer] = true;
            layer_requested_path[layer] = layer_texture_path[layer];
            layer_texture[layer] = layer_texture_path[layer].empty()
                ? nullptr
                : ResourceManager::get().load_async<TextureResource>(layer_texture_path[layer]);
        }

        const bool ready = layer_texture[layer] &&
                           layer_texture[layer]->get_state() == ResourceState::LoadedGPU &&
                           layer_texture[layer]->get_texture_id() != 0;
        out_layer_present[layer] = ready;

        glActiveTexture(GL_TEXTURE0 + first_layer_unit + layer);
        glBindTexture(GL_TEXTURE_2D, ready ? layer_texture[layer]->get_texture_id() : 0);
    }
    glActiveTexture(GL_TEXTURE0);
}

// --- Collision -------------------------------------------------------------

void TerrainComponent::destroy_collision() {
    if (body_id == JPH::BodyID::cInvalidBodyID) return;
    PhysicsEngine::get_instance().unregister_body(body_id);
    if (JPH::BodyInterface* bi = PhysicsEngine::get_instance().get_body_interface()) {
        bi->RemoveBody(JPH::BodyID(body_id));
        bi->DestroyBody(JPH::BodyID(body_id));
    }
    body_id = JPH::BodyID::cInvalidBodyID;
}

void TerrainComponent::rebuild_collision() {
    // Only rebuilt when a body already exists. Sculpting in the editor before Play
    // has nothing to keep in sync, and building one per brush step would be pure
    // waste.
    if (body_id == JPH::BodyID::cInvalidBodyID) return;
    destroy_collision();
    begin_play();
}

void TerrainComponent::begin_play() {
    destroy_collision();
    if (heights.empty()) return;

    JPH::BodyInterface* bi = PhysicsEngine::get_instance().get_body_interface();
    if (!bi) return;

    // Jolt's height field is defined as offset + scale * (x, sample, z) with x and z
    // stepping by one per sample, so the scale carries the cell size and the offset
    // moves the field's corner to where this terrain's corner actually is.
    const float step = world_size / static_cast<float>(resolution - 1);
    const Transform& t = placement();
    const JPH::Vec3 offset(static_cast<float>(t.position.x) - world_size * 0.5f,
                           static_cast<float>(t.position.y),
                           static_cast<float>(t.position.z) - world_size * 0.5f);
    const JPH::Vec3 scale(step, 1.0f, step);

    JPH::HeightFieldShapeSettings settings(heights.data(), offset, scale,
                                           static_cast<uint32_t>(resolution));
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        std::cerr << "[Terrain] Height field failed: " << result.GetError() << std::endl;
        return;
    }

    // Terrain is always static. A moving height field is not a thing Jolt supports,
    // and it is not a thing a level needs.
    JPH::BodyCreationSettings body_settings(result.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                            JPH::EMotionType::Static,
                                            PhysicsEngine::make_object_layer(collision_layer, false));
    body_settings.mFriction = 0.7f;

    JPH::BodyID id = bi->CreateAndAddBody(body_settings, JPH::EActivation::DontActivate);
    body_id = id.GetIndexAndSequenceNumber();
    PhysicsEngine::get_instance().register_body(body_id, owner, false);
}

// --- Persistence -----------------------------------------------------------

namespace {
// Identifies the file and its layout. A version rather than a bare magic so a
// terrain saved by an older build is refused cleanly instead of read as garbage.
constexpr uint32_t kTerrainMagic = 0x4C544552; // "LTER"
constexpr uint32_t kTerrainVersion = 1;
} // namespace

bool TerrainComponent::save_data(const std::string& filepath) const {
    if (heights.empty()) return false;

    std::error_code ec;
    std::filesystem::path parent = std::filesystem::path(filepath).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    std::ofstream file(filepath, std::ios::binary);
    if (!file) {
        std::cerr << "[Terrain] Could not write " << filepath << std::endl;
        return false;
    }

    const uint32_t magic = kTerrainMagic;
    const uint32_t version = kTerrainVersion;
    const uint32_t stored_resolution = static_cast<uint32_t>(resolution);
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&stored_resolution), sizeof(stored_resolution));
    file.write(reinterpret_cast<const char*>(&world_size), sizeof(world_size));
    file.write(reinterpret_cast<const char*>(heights.data()), heights.size() * sizeof(float));
    file.write(reinterpret_cast<const char*>(splat.data()), splat.size());
    file.write(reinterpret_cast<const char*>(foliage_coverage.data()), foliage_coverage.size());
    return static_cast<bool>(file);
}

bool TerrainComponent::load_data(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) return false;

    uint32_t magic = 0, version = 0, stored_resolution = 0;
    float stored_size = 0.0f;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&stored_resolution), sizeof(stored_resolution));
    file.read(reinterpret_cast<char*>(&stored_size), sizeof(stored_size));
    if (!file || magic != kTerrainMagic || version != kTerrainVersion) {
        std::cerr << "[Terrain] " << filepath << " is not a terrain file this build understands."
                  << std::endl;
        return false;
    }
    if (stored_resolution < static_cast<uint32_t>(kMinResolution) ||
        stored_resolution > static_cast<uint32_t>(kMaxResolution)) {
        std::cerr << "[Terrain] " << filepath << " has an implausible resolution ("
                  << stored_resolution << ")." << std::endl;
        return false;
    }

    const size_t sample_count = static_cast<size_t>(stored_resolution) * stored_resolution;
    std::vector<float> loaded_heights(sample_count);
    std::vector<unsigned char> loaded_splat(sample_count * 4);
    std::vector<unsigned char> loaded_foliage(sample_count);
    file.read(reinterpret_cast<char*>(loaded_heights.data()), sample_count * sizeof(float));
    file.read(reinterpret_cast<char*>(loaded_splat.data()), loaded_splat.size());
    file.read(reinterpret_cast<char*>(loaded_foliage.data()), loaded_foliage.size());
    if (!file) {
        std::cerr << "[Terrain] " << filepath << " ended early; it is truncated." << std::endl;
        return false;
    }

    resolution = static_cast<int>(stored_resolution);
    world_size = stored_size;
    heights = std::move(loaded_heights);
    splat = std::move(loaded_splat);
    foliage_coverage = std::move(loaded_foliage);
    data_path = filepath;

    rebuild_geometry();
    mark_splat_dirty();
    rebuild_collision();
    foliage_dirty = true;
    return true;
}

// --- Foliage ---------------------------------------------------------------

std::shared_ptr<MeshResource> TerrainComponent::get_foliage_mesh() const {
    if (foliage_mesh_path.empty()) {
        foliage_mesh.reset();
        foliage_requested_path.clear();
        return nullptr;
    }
    if (foliage_requested_path != foliage_mesh_path) {
        foliage_requested_path = foliage_mesh_path;
        foliage_mesh = ResourceManager::get().load_async<MeshResource>(foliage_mesh_path);
    }
    return foliage_mesh;
}

void TerrainComponent::regenerate_foliage() const {
    foliage_instances.clear();
    foliage_dirty = false;
    ++foliage_version;
    if (foliage_coverage.empty() || foliage_density <= 0.0f) return;

    const float step = world_size / static_cast<float>(resolution - 1);
    const float cell_area = step * step;
    // Expected instances in a fully covered cell. Clamped so a large density on a
    // coarse grid cannot ask for hundreds of instances in one cell.
    const float per_cell = std::min(16.0f, foliage_density * cell_area);
    const float slope_threshold = std::cos(std::max(0.0f, std::min(89.0f, foliage_max_slope_degrees)) * kDegToRad);

    for (int z = 0; z + 1 < resolution && static_cast<int>(foliage_instances.size()) < foliage_max_instances; ++z) {
        for (int x = 0; x + 1 < resolution; ++x) {
            const float coverage = foliage_coverage[index_of(x, z)] / 255.0f;
            if (coverage <= 0.0f) continue;

            const int candidates = static_cast<int>(std::ceil(per_cell));
            for (int candidate = 0; candidate < candidates; ++candidate) {
                if (static_cast<int>(foliage_instances.size()) >= foliage_max_instances) break;

                // Three independent hashes per candidate: whether it exists, and
                // where inside the cell it sits. Derived from the seed and the cell,
                // so the same terrain always scatters identically.
                const int salt = foliage_seed + candidate * 7919;
                const float acceptance = hash01(x, z, salt);
                // A per-cell expectation below one thins the population out rather
                // than rounding to zero, which is what makes sparse scatter - a few
                // trees per hundred square metres - expressible at all.
                const float chance = coverage * std::min(1.0f, per_cell);
                if (acceptance > chance) continue;

                const float jitter_x = hash01(x, z, salt + 1);
                const float jitter_z = hash01(x, z, salt + 2);
                const float local_x = sample_to_local(x) + jitter_x * step;
                const float local_z = sample_to_local(z) + jitter_z * step;

                const Vector3 normal = sample_normal(local_x, local_z);
                if (normal.y < slope_threshold) continue;

                const float yaw = hash01(x, z, salt + 3) * 6.2831853f;
                const float scale = foliage_min_scale +
                                    hash01(x, z, salt + 4) * std::max(0.0f, foliage_max_scale - foliage_min_scale);

                Matrix4x4 model = Matrix4x4::rotationY(yaw);
                model.m[0] *= scale; model.m[1] *= scale; model.m[2]  *= scale;
                model.m[4] *= scale; model.m[5] *= scale; model.m[6]  *= scale;
                model.m[8] *= scale; model.m[9] *= scale; model.m[10] *= scale;
                model.m[12] = local_x;
                model.m[13] = sample_height(local_x, local_z);
                model.m[14] = local_z;
                foliage_instances.push_back(model);
            }
        }
    }
}

const std::vector<Matrix4x4>& TerrainComponent::get_foliage_instances() const {
    if (foliage_dirty) regenerate_foliage();
    return foliage_instances;
}
