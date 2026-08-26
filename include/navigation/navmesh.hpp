#pragma once

#include "core/math.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Actor;

// Parameters the navigation build is run with. They describe the agent, not the
// world: the same level produces a different walkable surface for a rat and for a
// tank, and every one of these is a statement about what the agent can physically
// do.
struct NavBuildSettings {
    // Horizontal resolution, in metres. Everything below is measured in these, so
    // halving it quadruples both the build time and the memory.
    float cell_size = 0.3f;
    // Radius the agent occupies. Walkable surface is eroded by this, which is what
    // stops a path being generated that clips the agent's shoulder through a wall.
    float agent_radius = 0.4f;
    // Vertical space the agent needs. A surface with less headroom than this above
    // it is not walkable, however flat it is.
    float agent_height = 1.8f;
    // Surfaces steeper than this are walls.
    float max_slope_degrees = 45.0f;
    // Height difference the agent can climb between adjacent cells.
    float step_height = 0.4f;
    // Height difference the agent will step down between adjacent cells. Larger
    // than step_height because falling is easier than climbing.
    float max_drop = 2.0f;
    // Connects cells diagonally as well as orthogonally, which is what stops paths
    // looking like staircases across open ground. A diagonal is only used when both
    // of its orthogonal components are also walkable, so an agent never cuts the
    // corner of a wall.
    bool allow_diagonals = true;
};

// The scene's navigation data.
//
// Built by sampling every walkable surface onto a regular grid in the horizontal
// plane, keeping every distinct height a column has - so a bridge over a road
// produces two independent walkable levels in the same column rather than one of
// them erasing the other. Paths are found over that graph with A* and then
// straightened, because a raw grid path is a staircase and no character should walk
// one.
//
// A grid rather than the convex-polygon representation a full Recast build would
// produce: the polygon form pays off in memory and in path smoothness for very
// large worlds, and costs a region-growing, contour-tracing and triangulation
// pipeline to get there. The grid gives the same agent radius, step height, slope
// limit and multi-level support, which is what gameplay actually asks of it.
class NavMesh {
public:
    static NavMesh& get();

    // Rebuilds from the scene. Every actor with mesh geometry contributes, except
    // trigger volumes and anything carrying a character controller - neither is
    // ground to walk on. Returns false and explains itself in out_report on failure;
    // out_report also carries the statistics for a successful build.
    bool build(const std::vector<std::shared_ptr<Actor>>& actors,
               const NavBuildSettings& settings,
               std::string& out_report);

    void clear();
    bool is_built() const { return !nodes.empty(); }
    int  get_node_count() const { return static_cast<int>(nodes.size()); }
    int  get_usable_node_count() const { return usable_node_count; }
    const NavBuildSettings& get_settings() const { return settings; }
    // Summary of the last build, for the editor to display.
    const std::string& get_last_report() const { return last_report; }

    // World-space path from start to end, including both endpoints. Returns false if
    // either end is off the navigable surface or no route exists.
    bool find_path(const DVector3& start, const DVector3& end,
                   std::vector<DVector3>& out_points) const;

    // Nearest navigable point to `near`, searched outward up to max_distance
    // horizontally. This is how a destination clicked in mid-air, or a spawn point
    // hovering slightly above the floor, is turned into something reachable.
    bool sample_position(const DVector3& near, float max_distance, DVector3& out_point) const;

    // Whether an agent can walk straight between two points without leaving the
    // navigable surface. Used to straighten paths, and useful on its own for an AI
    // deciding whether it needs a path at all.
    bool line_of_sight(const DVector3& from, const DVector3& to) const;

    // World-space centres of the navigable cells, for debug drawing. Rebuilt only
    // by build(), so this is a cheap read.
    const std::vector<Vector3>& get_debug_cells() const { return debug_cells; }
    float get_cell_size() const { return settings.cell_size; }

private:
    NavMesh() = default;
    ~NavMesh() = default;
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // One walkable surface in one grid column. A column can hold several - the road
    // and the bridge above it - which is what makes this more than a heightmap.
    struct NavNode {
        float y = 0.0f;
        int column = 0;
        // Index into `nodes` per direction, or -1. Directions 0-3 are orthogonal
        // (+x, -x, +z, -z) and 4-7 diagonal.
        int neighbours[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        // Cells to the nearest edge of the walkable surface. Compared against the
        // agent radius to decide whether an agent actually fits here.
        int clearance = 0;
        bool usable = false;
    };

    int  world_to_column(double x, double z) const;
    void column_to_world(int column, float& out_x, float& out_z) const;
    // Node in this column closest in height to `y`, or -1. Only usable nodes are
    // considered, because an agent cannot stand where it does not fit.
    int  node_in_column(int column, double y, float tolerance) const;
    int  nearest_node(const DVector3& position, float max_distance) const;
    // The body of line_of_sight without the lock, so path straightening - which
    // already holds it - can use it without deadlocking against itself.
    bool line_of_sight_internal(const DVector3& from, const DVector3& to) const;

    NavBuildSettings settings;
    std::string last_report;

    // Grid extents, in cells, and the world position of cell (0, 0)'s minimum corner.
    int grid_width = 0;
    int grid_depth = 0;
    double origin_x = 0.0;
    double origin_z = 0.0;

    std::vector<NavNode> nodes;
    // Compressed per-column node lists: nodes for column c are
    // column_nodes[column_start[c] .. column_start[c + 1]), sorted by height.
    // A flat pair of arrays rather than a vector per column, because a 100m world at
    // 0.3m resolution has 110,000 columns and almost all of them hold one node.
    std::vector<int> column_start;
    std::vector<int> column_nodes;

    int usable_node_count = 0;
    std::vector<Vector3> debug_cells;

    // A* scratch, kept between queries so a search does not allocate. Stamped rather
    // than cleared: zeroing 100,000 entries per query costs more than the search.
    mutable std::vector<float> g_score;
    mutable std::vector<int> came_from;
    mutable std::vector<unsigned int> visit_stamp;
    mutable unsigned int current_stamp = 0;
    // Scripts can request a path from the parallel actor tick, so the scratch above
    // has to be serialised. Searches are short; contention is not a concern.
    mutable std::mutex query_mutex;
};
