#include "navigation/navmesh.hpp"

#include "world/actor.hpp"
#include "world/static_mesh_component.hpp"
#include "world/physics_attribute.hpp"
#include "world/character_controller_component.hpp"
#include "core/mesh_resource.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <queue>
#include <sstream>

namespace {

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

// Orthogonal first so the erosion pass, which must not travel diagonally, can just
// take the first four.
constexpr int kDirX[8] = {  1, -1,  0,  0,  1,  1, -1, -1 };
constexpr int kDirZ[8] = {  0,  0,  1, -1,  1, -1,  1, -1 };
// For each diagonal, the two orthogonal directions it is made of. A diagonal is only
// connected when both of those are, which is what stops an agent slipping through
// the corner where two walls meet.
constexpr int kDiagonalComponents[4][2] = { { 0, 2 }, { 0, 3 }, { 1, 2 }, { 1, 3 } };

// One surface sample in one grid column.
struct ColumnHit {
    float y = 0.0f;
    bool floor = false; // faces up steeply enough to stand on
};

// Should this actor's geometry be part of the walkable world?
bool contributes_to_navigation(Actor* actor) {
    if (!actor) return false;
    // A character is a thing that walks on the navmesh, not part of it.
    if (actor->get_component<CharacterControllerComponent>()) return false;
    // A trigger is a volume you pass through; building floor out of it would put a
    // walkable surface in mid-air wherever a designer placed one.
    if (auto* physics = actor->get_component<PhysicsAttribute>()) {
        if (physics->is_trigger) return false;
    }
    return true;
}

} // namespace

NavMesh& NavMesh::get() {
    static NavMesh instance;
    return instance;
}

void NavMesh::clear() {
    nodes.clear();
    column_start.clear();
    column_nodes.clear();
    debug_cells.clear();
    usable_node_count = 0;
    grid_width = 0;
    grid_depth = 0;
    g_score.clear();
    came_from.clear();
    visit_stamp.clear();
    current_stamp = 0;
}

int NavMesh::world_to_column(double x, double z) const {
    const int cx = static_cast<int>(std::floor((x - origin_x) / settings.cell_size));
    const int cz = static_cast<int>(std::floor((z - origin_z) / settings.cell_size));
    if (cx < 0 || cz < 0 || cx >= grid_width || cz >= grid_depth) return -1;
    return cz * grid_width + cx;
}

void NavMesh::column_to_world(int column, float& out_x, float& out_z) const {
    const int cx = column % grid_width;
    const int cz = column / grid_width;
    out_x = static_cast<float>(origin_x + (cx + 0.5) * settings.cell_size);
    out_z = static_cast<float>(origin_z + (cz + 0.5) * settings.cell_size);
}

// --- Build -----------------------------------------------------------------

bool NavMesh::build(const std::vector<std::shared_ptr<Actor>>& actors,
                    const NavBuildSettings& build_settings,
                    std::string& out_report) {
    std::lock_guard<std::mutex> lock(query_mutex);
    clear();
    settings = build_settings;
    if (settings.cell_size < 0.02f) settings.cell_size = 0.02f;

    // --- Gather world-space triangles -------------------------------------
    struct Tri { Vector3 a, b, c; };
    std::vector<Tri> triangles;

    Vector3 world_min = { 1e30f, 1e30f, 1e30f };
    Vector3 world_max = { -1e30f, -1e30f, -1e30f };

    for (const auto& actor : actors) {
        if (!contributes_to_navigation(actor.get())) continue;

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

            const Matrix4x4 model = mesh->transform.get_matrix();
            for (size_t i = 0; i + 2 < indices->size(); i += 3) {
                const unsigned int i0 = (*indices)[i], i1 = (*indices)[i + 1], i2 = (*indices)[i + 2];
                if (i0 >= vertices->size() || i1 >= vertices->size() || i2 >= vertices->size()) continue;

                Tri tri;
                tri.a = model * (*vertices)[i0].position;
                tri.b = model * (*vertices)[i1].position;
                tri.c = model * (*vertices)[i2].position;
                triangles.push_back(tri);

                for (const Vector3* p : { &tri.a, &tri.b, &tri.c }) {
                    world_min.x = std::min(world_min.x, p->x);
                    world_min.y = std::min(world_min.y, p->y);
                    world_min.z = std::min(world_min.z, p->z);
                    world_max.x = std::max(world_max.x, p->x);
                    world_max.y = std::max(world_max.y, p->y);
                    world_max.z = std::max(world_max.z, p->z);
                }
            }
        }
    }

    if (triangles.empty()) {
        out_report = "No mesh geometry to build from. Meshes still streaming in do not count.";
        last_report = out_report;
        return false;
    }

    // One cell of padding so a surface ending exactly on the boundary still has a
    // column of its own.
    origin_x = static_cast<double>(world_min.x) - settings.cell_size;
    origin_z = static_cast<double>(world_min.z) - settings.cell_size;
    const double span_x = static_cast<double>(world_max.x) - origin_x + settings.cell_size;
    const double span_z = static_cast<double>(world_max.z) - origin_z + settings.cell_size;

    grid_width = static_cast<int>(std::ceil(span_x / settings.cell_size));
    grid_depth = static_cast<int>(std::ceil(span_z / settings.cell_size));
    if (grid_width <= 0 || grid_depth <= 0) {
        out_report = "Scene bounds are degenerate; nothing to navigate.";
        last_report = out_report;
        clear();
        return false;
    }

    // A hard ceiling on the grid, because cell_size is a number a person types and
    // one wrong digit there asks for tens of gigabytes without complaint. Four
    // million columns is a 600m square at the default 30cm resolution, and costs
    // about a hundred megabytes while the samples are being collected.
    const long long column_count = static_cast<long long>(grid_width) * grid_depth;
    constexpr long long kMaxColumns = 4'000'000;
    if (column_count > kMaxColumns) {
        std::ostringstream message;
        message << "Grid would be " << grid_width << " x " << grid_depth << " cells ("
                << column_count << "). Increase the cell size.";
        out_report = message.str();
        last_report = out_report;
        clear();
        return false;
    }

    // --- Sample every surface height in every column ----------------------
    // For each triangle, walk the cells its horizontal footprint covers and record
    // the height at each cell centre it actually contains. Sampling at cell centres
    // rather than conservatively voxelising means a surface is represented exactly
    // where an agent would stand on it, and costs one interpolation per cell.
    std::vector<std::vector<ColumnHit>> column_hits(static_cast<size_t>(column_count));

    const float slope_threshold = std::cos(std::max(0.0f, std::min(89.0f, settings.max_slope_degrees)) * kDegToRad);

    for (const Tri& tri : triangles) {
        const float min_x = std::min(tri.a.x, std::min(tri.b.x, tri.c.x));
        const float max_x = std::max(tri.a.x, std::max(tri.b.x, tri.c.x));
        const float min_z = std::min(tri.a.z, std::min(tri.b.z, tri.c.z));
        const float max_z = std::max(tri.a.z, std::max(tri.b.z, tri.c.z));

        int cx0 = static_cast<int>(std::floor((min_x - origin_x) / settings.cell_size));
        int cx1 = static_cast<int>(std::floor((max_x - origin_x) / settings.cell_size));
        int cz0 = static_cast<int>(std::floor((min_z - origin_z) / settings.cell_size));
        int cz1 = static_cast<int>(std::floor((max_z - origin_z) / settings.cell_size));
        cx0 = std::max(0, cx0); cz0 = std::max(0, cz0);
        cx1 = std::min(grid_width - 1, cx1); cz1 = std::min(grid_depth - 1, cz1);
        if (cx0 > cx1 || cz0 > cz1) continue;

        // Barycentric setup, in the horizontal plane. A triangle standing exactly
        // vertical projects to a line and has no interior to sample - which is
        // correct, since nothing can stand on it.
        const float e0x = tri.b.x - tri.a.x, e0z = tri.b.z - tri.a.z;
        const float e1x = tri.c.x - tri.a.x, e1z = tri.c.z - tri.a.z;
        const float denominator = e0x * e1z - e1x * e0z;
        if (std::abs(denominator) < 1e-9f) continue;
        const float inv_denominator = 1.0f / denominator;

        // Upward component of the triangle normal. Winding decides the sign, and a
        // downward-facing triangle is a ceiling: recorded, because it takes headroom
        // away from the floor below, but never walkable itself.
        const Vector3 normal = Vector3::cross(tri.b - tri.a, tri.c - tri.a).normalized();

        for (int cz = cz0; cz <= cz1; ++cz) {
            const float pz = static_cast<float>(origin_z + (cz + 0.5) * settings.cell_size);
            for (int cx = cx0; cx <= cx1; ++cx) {
                const float px = static_cast<float>(origin_x + (cx + 0.5) * settings.cell_size);

                const float v2x = px - tri.a.x;
                const float v2z = pz - tri.a.z;
                const float u = (v2x * e1z - e1x * v2z) * inv_denominator;
                const float v = (e0x * v2z - v2x * e0z) * inv_denominator;
                if (u < 0.0f || v < 0.0f || u + v > 1.0f) continue;

                ColumnHit hit;
                hit.y = tri.a.y + u * (tri.b.y - tri.a.y) + v * (tri.c.y - tri.a.y);
                hit.floor = (normal.y >= slope_threshold);
                column_hits[static_cast<size_t>(cz) * grid_width + cx].push_back(hit);
            }
        }
    }

    // --- Turn samples into nodes ------------------------------------------
    column_start.assign(static_cast<size_t>(column_count) + 1, 0);
    nodes.clear();
    column_nodes.clear();

    for (long long column = 0; column < column_count; ++column) {
        column_start[static_cast<size_t>(column)] = static_cast<int>(column_nodes.size());

        auto& hits = column_hits[static_cast<size_t>(column)];
        if (hits.empty()) continue;
        std::sort(hits.begin(), hits.end(),
                  [](const ColumnHit& a, const ColumnHit& b) { return a.y < b.y; });

        for (size_t i = 0; i < hits.size(); ++i) {
            if (!hits[i].floor) continue;

            // Headroom is the gap to the next distinct surface above. Surfaces within
            // a hair of this one are the same floor sampled twice - a seam between
            // two triangles - and must not be read as a ceiling one millimetre up.
            float headroom = 1e30f;
            for (size_t j = i + 1; j < hits.size(); ++j) {
                if (hits[j].y - hits[i].y < 0.05f) continue;
                headroom = hits[j].y - hits[i].y;
                break;
            }
            if (headroom < settings.agent_height) continue;

            // Two samples a few centimetres apart are the same floor picked up
            // twice - a seam where two triangles meet, or coplanar geometry - and
            // keeping both would double the node count across the whole level.
            if (!nodes.empty() && !column_nodes.empty() &&
                static_cast<int>(column_nodes.size()) > column_start[static_cast<size_t>(column)] &&
                std::abs(nodes[column_nodes.back()].y - hits[i].y) < 0.05f) {
                continue;
            }

            NavNode node;
            node.y = hits[i].y;
            node.column = static_cast<int>(column);
            column_nodes.push_back(static_cast<int>(nodes.size()));
            nodes.push_back(node);
        }
    }
    column_start[static_cast<size_t>(column_count)] = static_cast<int>(column_nodes.size());

    if (nodes.empty()) {
        out_report = "No walkable surface found. Check the slope limit and agent height.";
        last_report = out_report;
        clear();
        return false;
    }

    // --- Connect neighbours -----------------------------------------------
    // Orthogonal links first: the diagonal test needs to know which of them exist.
    for (size_t index = 0; index < nodes.size(); ++index) {
        NavNode& node = nodes[index];
        const int cx = node.column % grid_width;
        const int cz = node.column / grid_width;

        for (int direction = 0; direction < 4; ++direction) {
            const int nx = cx + kDirX[direction];
            const int nz = cz + kDirZ[direction];
            if (nx < 0 || nz < 0 || nx >= grid_width || nz >= grid_depth) continue;

            const int neighbour_column = nz * grid_width + nx;
            int best = -1;
            float best_difference = 1e30f;
            for (int slot = column_start[neighbour_column]; slot < column_start[neighbour_column + 1]; ++slot) {
                const int candidate = column_nodes[slot];
                const float rise = nodes[candidate].y - node.y;
                // Climbing is limited by the step height, dropping by how far the
                // agent is willing to fall. They are different numbers because they
                // are different actions.
                if (rise > settings.step_height) continue;
                if (-rise > settings.max_drop) continue;
                const float difference = std::abs(rise);
                if (difference < best_difference) {
                    best_difference = difference;
                    best = candidate;
                }
            }
            node.neighbours[direction] = best;
        }
    }

    if (settings.allow_diagonals) {
        for (size_t index = 0; index < nodes.size(); ++index) {
            NavNode& node = nodes[index];
            const int cx = node.column % grid_width;
            const int cz = node.column / grid_width;

            for (int diagonal = 0; diagonal < 4; ++diagonal) {
                const int direction = 4 + diagonal;
                // Both orthogonal halves must exist, or this diagonal squeezes
                // between two blocked cells - through the corner of a wall.
                if (node.neighbours[kDiagonalComponents[diagonal][0]] < 0) continue;
                if (node.neighbours[kDiagonalComponents[diagonal][1]] < 0) continue;

                const int nx = cx + kDirX[direction];
                const int nz = cz + kDirZ[direction];
                if (nx < 0 || nz < 0 || nx >= grid_width || nz >= grid_depth) continue;

                const int neighbour_column = nz * grid_width + nx;
                int best = -1;
                float best_difference = 1e30f;
                for (int slot = column_start[neighbour_column]; slot < column_start[neighbour_column + 1]; ++slot) {
                    const int candidate = column_nodes[slot];
                    const float rise = nodes[candidate].y - node.y;
                    if (rise > settings.step_height) continue;
                    if (-rise > settings.max_drop) continue;
                    const float difference = std::abs(rise);
                    if (difference < best_difference) {
                        best_difference = difference;
                        best = candidate;
                    }
                }
                node.neighbours[direction] = best;
            }
        }
    }

    // --- Erode by the agent radius ----------------------------------------
    // A breadth-first distance transform from the edge of the walkable surface. A
    // node further from an edge than the agent's radius is one the agent fits on;
    // anything closer would put its shoulder through a wall.
    {
        std::vector<int> distance(nodes.size(), -1);
        std::queue<int> frontier;
        for (size_t index = 0; index < nodes.size(); ++index) {
            bool on_border = false;
            for (int direction = 0; direction < 4 && !on_border; ++direction) {
                if (nodes[index].neighbours[direction] < 0) on_border = true;
            }
            if (on_border) {
                distance[index] = 0;
                frontier.push(static_cast<int>(index));
            }
        }
        while (!frontier.empty()) {
            const int index = frontier.front();
            frontier.pop();
            for (int direction = 0; direction < 4; ++direction) {
                const int neighbour = nodes[index].neighbours[direction];
                if (neighbour < 0 || distance[neighbour] >= 0) continue;
                distance[neighbour] = distance[index] + 1;
                frontier.push(neighbour);
            }
        }

        // A surface with no border at all - a closed torus, or a floor filling the
        // whole grid - leaves every distance at -1, which means unbounded room.
        const int required = std::max(0,
            static_cast<int>(std::ceil(settings.agent_radius / settings.cell_size)) - 1);

        usable_node_count = 0;
        for (size_t index = 0; index < nodes.size(); ++index) {
            nodes[index].clearance = (distance[index] < 0) ? required : distance[index];
            nodes[index].usable = (nodes[index].clearance >= required);
            if (nodes[index].usable) ++usable_node_count;
        }

        // Links into eroded-away nodes are dropped, so a search never has to test
        // usability while it runs.
        for (NavNode& node : nodes) {
            for (int direction = 0; direction < 8; ++direction) {
                const int neighbour = node.neighbours[direction];
                if (neighbour >= 0 && !nodes[neighbour].usable) node.neighbours[direction] = -1;
            }
        }
    }

    // --- Debug geometry ----------------------------------------------------
    debug_cells.clear();
    debug_cells.reserve(static_cast<size_t>(usable_node_count));
    for (const NavNode& node : nodes) {
        if (!node.usable) continue;
        float x, z;
        column_to_world(node.column, x, z);
        debug_cells.push_back({ x, node.y, z });
    }

    g_score.assign(nodes.size(), 0.0f);
    came_from.assign(nodes.size(), -1);
    visit_stamp.assign(nodes.size(), 0);
    current_stamp = 0;

    std::ostringstream message;
    message << grid_width << " x " << grid_depth << " cells at " << settings.cell_size << "m; "
            << nodes.size() << " walkable surfaces, " << usable_node_count
            << " reachable by a " << settings.agent_radius << "m agent";
    if (usable_node_count == 0) {
        message << ". Nothing fits - reduce the agent radius or the cell size.";
    }
    out_report = message.str();
    last_report = out_report;
    return usable_node_count > 0;
}

// --- Queries ---------------------------------------------------------------

int NavMesh::node_in_column(int column, double y, float tolerance) const {
    if (column < 0 || column + 1 >= static_cast<int>(column_start.size())) return -1;

    int best = -1;
    float best_difference = tolerance;
    for (int slot = column_start[column]; slot < column_start[column + 1]; ++slot) {
        const int candidate = column_nodes[slot];
        if (!nodes[candidate].usable) continue;
        const float difference = static_cast<float>(std::abs(nodes[candidate].y - y));
        if (difference <= best_difference) {
            best_difference = difference;
            best = candidate;
        }
    }
    return best;
}

int NavMesh::nearest_node(const DVector3& position, float max_distance) const {
    if (nodes.empty() || grid_width <= 0) return -1;

    const int cx = static_cast<int>(std::floor((position.x - origin_x) / settings.cell_size));
    const int cz = static_cast<int>(std::floor((position.z - origin_z) / settings.cell_size));
    const int max_rings = std::max(1, static_cast<int>(std::ceil(max_distance / settings.cell_size)));

    int best = -1;
    double best_distance_squared = static_cast<double>(max_distance) * max_distance;

    // Outward ring search. Stopping at the first ring that produced a hit is not
    // enough - a node in the next ring can still be closer in 3D if it sits at a
    // much better height - so one extra ring is always examined.
    int rings_after_hit = -1;
    for (int ring = 0; ring <= max_rings; ++ring) {
        if (rings_after_hit >= 0 && ring > rings_after_hit + 1) break;

        for (int dz = -ring; dz <= ring; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                // Only the perimeter of this ring; the interior was covered already.
                if (ring > 0 && std::abs(dx) != ring && std::abs(dz) != ring) continue;

                const int nx = cx + dx;
                const int nz = cz + dz;
                if (nx < 0 || nz < 0 || nx >= grid_width || nz >= grid_depth) continue;

                const int column = nz * grid_width + nx;
                for (int slot = column_start[column]; slot < column_start[column + 1]; ++slot) {
                    const int candidate = column_nodes[slot];
                    if (!nodes[candidate].usable) continue;

                    float wx, wz;
                    column_to_world(column, wx, wz);
                    const double ddx = wx - position.x;
                    const double ddy = nodes[candidate].y - position.y;
                    const double ddz = wz - position.z;
                    const double distance_squared = ddx * ddx + ddy * ddy + ddz * ddz;
                    if (distance_squared < best_distance_squared) {
                        best_distance_squared = distance_squared;
                        best = candidate;
                        if (rings_after_hit < 0) rings_after_hit = ring;
                    }
                }
            }
        }
    }
    return best;
}

bool NavMesh::sample_position(const DVector3& query, float max_distance, DVector3& out_point) const {
    std::lock_guard<std::mutex> lock(query_mutex);
    const int node = nearest_node(query, max_distance);
    if (node < 0) return false;

    float x, z;
    column_to_world(nodes[node].column, x, z);
    out_point = { static_cast<double>(x), static_cast<double>(nodes[node].y), static_cast<double>(z) };
    return true;
}

bool NavMesh::line_of_sight(const DVector3& from, const DVector3& to) const {
    std::lock_guard<std::mutex> lock(query_mutex);
    return line_of_sight_internal(from, to);
}

bool NavMesh::line_of_sight_internal(const DVector3& from, const DVector3& to) const {
    if (nodes.empty()) return false;

    const double dx = to.x - from.x;
    const double dz = to.z - from.z;
    const double horizontal = std::sqrt(dx * dx + dz * dz);
    if (horizontal < 1e-6) return true;

    // Half a cell per step, so no cell along the line can be skipped over.
    const int steps = std::max(2, static_cast<int>(std::ceil(horizontal / (settings.cell_size * 0.5))));
    // The line is allowed to rise or fall by a step's worth between samples, which is
    // the same allowance the graph edges were built with.
    const float tolerance = settings.step_height + settings.cell_size;

    for (int step = 0; step <= steps; ++step) {
        const double t = static_cast<double>(step) / steps;
        const double x = from.x + dx * t;
        const double z = from.z + dz * t;
        const double y = from.y + (to.y - from.y) * t;

        const int column = world_to_column(x, z);
        if (column < 0) return false;
        if (node_in_column(column, y, tolerance) < 0) return false;
    }
    return true;
}

bool NavMesh::find_path(const DVector3& start, const DVector3& end,
                        std::vector<DVector3>& out_points) const {
    std::lock_guard<std::mutex> lock(query_mutex);
    out_points.clear();
    if (nodes.empty()) return false;

    // Both ends are snapped onto the surface. A destination clicked in mid-air, or a
    // spawn hovering a centimetre above the floor, is the normal case, not an error.
    const float snap_distance = std::max(2.0f, settings.cell_size * 8.0f);
    const int start_node = nearest_node(start, snap_distance);
    const int end_node = nearest_node(end, snap_distance);
    if (start_node < 0 || end_node < 0) return false;

    if (start_node == end_node) {
        out_points.push_back(start);
        out_points.push_back(end);
        return true;
    }

    if (g_score.size() != nodes.size()) {
        g_score.assign(nodes.size(), 0.0f);
        came_from.assign(nodes.size(), -1);
        visit_stamp.assign(nodes.size(), 0);
        current_stamp = 0;
    }

    // Stamped rather than cleared: resetting three arrays of every node in the level
    // costs more than most searches do.
    ++current_stamp;
    if (current_stamp == 0) {
        // Wrapped after four billion queries; the stale stamps are now ambiguous.
        std::fill(visit_stamp.begin(), visit_stamp.end(), 0);
        current_stamp = 1;
    }

    auto node_world = [this](int index, double& x, double& y, double& z) {
        float wx, wz;
        column_to_world(nodes[index].column, wx, wz);
        x = wx;
        y = nodes[index].y;
        z = wz;
    };

    auto heuristic = [&](int a, int b) {
        double ax, ay, az, bx, by, bz;
        node_world(a, ax, ay, az);
        node_world(b, bx, by, bz);
        const double dx = ax - bx, dy = ay - by, dz = az - bz;
        return static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
    };

    struct Candidate {
        float f;
        int node;
        bool operator>(const Candidate& other) const { return f > other.f; }
    };
    std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>> open;

    visit_stamp[start_node] = current_stamp;
    g_score[start_node] = 0.0f;
    came_from[start_node] = -1;
    open.push({ heuristic(start_node, end_node), start_node });

    bool found = false;
    while (!open.empty()) {
        const Candidate current = open.top();
        open.pop();

        if (current.node == end_node) { found = true; break; }
        // A node can be pushed more than once with different scores; the stale copies
        // are recognised by carrying a worse f than the score now recorded.
        if (current.f > g_score[current.node] + heuristic(current.node, end_node) + 1e-3f) continue;

        double cx, cy, cz;
        node_world(current.node, cx, cy, cz);

        for (int direction = 0; direction < 8; ++direction) {
            const int neighbour = nodes[current.node].neighbours[direction];
            if (neighbour < 0) continue;

            double nx, ny, nz;
            node_world(neighbour, nx, ny, nz);
            const double ddx = nx - cx, ddy = ny - cy, ddz = nz - cz;
            const float step_cost = static_cast<float>(std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz));
            const float tentative = g_score[current.node] + step_cost;

            if (visit_stamp[neighbour] == current_stamp && tentative >= g_score[neighbour]) continue;

            visit_stamp[neighbour] = current_stamp;
            g_score[neighbour] = tentative;
            came_from[neighbour] = current.node;
            open.push({ tentative + heuristic(neighbour, end_node), neighbour });
        }
    }

    if (!found) return false;

    // Walk the parent chain back and reverse it.
    std::vector<int> chain;
    for (int node = end_node; node >= 0; node = came_from[node]) {
        chain.push_back(node);
        if (node == start_node) break;
        if (chain.size() > nodes.size()) return false; // corrupt chain; refuse rather than loop
    }
    std::reverse(chain.begin(), chain.end());

    std::vector<DVector3> raw;
    raw.reserve(chain.size() + 2);
    raw.push_back(start);
    for (int node : chain) {
        double x, y, z;
        node_world(node, x, y, z);
        raw.push_back({ x, y, z });
    }
    raw.push_back(end);

    // --- Straighten --------------------------------------------------------
    // A grid path is a staircase. Skipping ahead to the furthest point still in
    // walkable line of sight turns it back into the handful of corners a person
    // would actually walk. The lookahead is bounded so a long path cannot make this
    // quadratic in its own length.
    constexpr int kMaxLookahead = 48;
    out_points.push_back(raw.front());
    size_t index = 0;
    while (index + 1 < raw.size()) {
        size_t furthest = index + 1;
        const size_t limit = std::min(raw.size() - 1, index + kMaxLookahead);
        for (size_t candidate = limit; candidate > index + 1; --candidate) {
            if (line_of_sight_internal(raw[index], raw[candidate])) {
                furthest = candidate;
                break;
            }
        }
        out_points.push_back(raw[furthest]);
        index = furthest;
    }

    return out_points.size() >= 2;
}
