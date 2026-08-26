#pragma once

#include "core/math.hpp"
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <string>
#include <vector>
#include <functional>

// Forward declarations for Jolt
namespace JPH {
    class PhysicsSystem;
    class JobSystemThreadPool;
    class TempAllocator;
    class TempAllocatorImpl;
    class BodyInterface;
    class BodyID;
}

class Actor;

// What a query hit. Distance and the surface it landed on are what gameplay
// actually asks for - a decal needs the point and the normal, a projectile needs
// the actor it struck, and the old distance-only raycast could answer none of it.
struct RaycastHit {
    Actor* actor = nullptr;
    uint32_t body_id = 0xFFFFFFFFu;
    Vector3 point = { 0.0f, 0.0f, 0.0f };
    // Outward surface normal at the point of impact.
    Vector3 normal = { 0.0f, 0.0f, 0.0f };
    float distance = 0.0f;
    // Gameplay layer of the body that was hit.
    int layer = 0;
};

// One collision or trigger transition for a body pair, as resolved for gameplay.
struct PhysicsContactEvent {
    enum class Phase { Enter, Stay, Exit };

    Phase phase = Phase::Enter;
    uint32_t body_a = 0;
    uint32_t body_b = 0;
    Vector3 point = { 0.0f, 0.0f, 0.0f };
    // Points out of body_a toward body_b.
    Vector3 normal = { 0.0f, 0.0f, 0.0f };
    float approach_speed = 0.0f;
    // True when either body is a sensor, which makes this a trigger rather than a
    // collision. Sensors report contacts but generate no collision response.
    bool is_trigger = false;
};

class PhysicsEngine {
public:
    // --- Collision layers ----------------------------------------------------
    // Gameplay names a layer with a small integer, and a symmetric matrix decides
    // which pairs of layers interact at all. This is what expresses "bullets pass
    // through the player" or "debris only collides with the world" without any
    // per-contact filtering in gameplay code.
    //
    // Jolt needs each body's *object layer* to encode both the gameplay layer and
    // whether the body moves, because it uses that to skip static-versus-static
    // pairs entirely. The two are packed together as
    //     object layer = gameplay layer * 2 + (moving ? 1 : 0)
    // which is why LAYER_NON_MOVING and LAYER_MOVING below - the two values this
    // engine used before layers existed - are still exactly correct: they are the
    // static and moving halves of gameplay layer 0.
    static constexpr int kLayerCount = 32;

    static constexpr uint16_t LAYER_NON_MOVING = 0;
    static constexpr uint16_t LAYER_MOVING = 1;

    static uint16_t make_object_layer(int gameplay_layer, bool is_moving) {
        if (gameplay_layer < 0 || gameplay_layer >= kLayerCount) gameplay_layer = 0;
        return static_cast<uint16_t>(gameplay_layer * 2 + (is_moving ? 1 : 0));
    }
    static int gameplay_layer_of(uint16_t object_layer) { return object_layer >> 1; }
    static bool object_layer_is_moving(uint16_t object_layer) { return (object_layer & 1) != 0; }

    // The matrix. Symmetric: setting a against b sets b against a, because a
    // one-directional collision is not a thing the solver can represent.
    static bool layers_should_collide(int layer_a, int layer_b);
    static void set_layers_collide(int layer_a, int layer_b, bool enabled);
    // Bit mask of every layer this one collides with, for query filtering.
    static uint32_t get_layer_mask(int layer);

    static const std::string& get_layer_name(int layer);
    static void set_layer_name(int layer, const std::string& name);
    // Everything collides with everything, which is how a project with no layer
    // setup behaves and what the matrix starts as.
    static void reset_layers();

    // Downward acceleration applied to characters. Read from the physics system so a
    // character falls at the same rate as a rigid body rather than at its own guess.
    float get_gravity_y() const;

    static PhysicsEngine& get_instance();

    void initialize();
    void cleanup();
    
    // Step the physics simulation
    void tick(float delta_time);

    // --- Queries -------------------------------------------------------------
    // Perform a raycast. Returns true if hit, sets out_distance to hit distance.
    // Kept for callers that only need the distance; prefer the RaycastHit form.
    bool raycast(double start_x, double start_y, double start_z, double dir_x, double dir_y, double dir_z, float max_distance, float& out_distance);

    // Closest hit along the ray. layer_mask is a bit per gameplay layer; a body
    // whose layer is not in the mask is invisible to the query, which is how a
    // camera ray ignores triggers or an AI ignores its own team.
    bool raycast(const DVector3& origin, const Vector3& direction, float max_distance,
                 RaycastHit& out_hit, uint32_t layer_mask = 0xFFFFFFFFu) const;

    // Every hit along the ray, nearest first. For a shot that passes through glass,
    // or for counting what is between two points.
    bool raycast_all(const DVector3& origin, const Vector3& direction, float max_distance,
                     std::vector<RaycastHit>& out_hits, uint32_t layer_mask = 0xFFFFFFFFu) const;

    // Sweeps a sphere along the ray. This is what a thick projectile or a camera
    // boom needs: a zero-width ray slips through gaps the object itself could not.
    bool sphere_cast(const DVector3& origin, const Vector3& direction, float radius,
                     float max_distance, RaycastHit& out_hit,
                     uint32_t layer_mask = 0xFFFFFFFFu) const;

    // Every actor whose collider overlaps the sphere. The workhorse for explosions,
    // area-of-effect and proximity checks.
    bool overlap_sphere(const DVector3& center, float radius, std::vector<Actor*>& out_actors,
                        uint32_t layer_mask = 0xFFFFFFFFu) const;

    // Accessors for Jolt components needed by PhysicsAttribute
    JPH::PhysicsSystem* get_physics_system() { return physics_system.get(); }
    JPH::BodyInterface* get_body_interface();

    // Scratch allocator for character collision queries. This is a linear allocator
    // and is NOT thread safe, so every caller has to be on the same thread - which is
    // why character updates run sequentially on the logic thread rather than inside
    // the parallel actor tick.
    JPH::TempAllocator* get_temp_allocator();

    bool is_ready() const { return is_initialized; }

    // --- Contact reporting ---------------------------------------------------
    // Bodies are registered so a contact can name the actor it belongs to. Jolt's
    // callbacks only carry BodyIDs, and by the time a contact ends the body may
    // already have been destroyed, so the mapping cannot be a raw pointer stored on
    // the body itself.
    void register_body(uint32_t body_id, Actor* actor, bool is_sensor);
    void unregister_body(uint32_t body_id);
    Actor* actor_for_body(uint32_t body_id) const;

    // Turns the raw per-sub-shape contacts Jolt reported during the last tick() into
    // one Enter/Stay/Exit transition per body pair, on the thread that owns the
    // scene. The result is owned by the engine and stays valid until the next tick.
    //
    // Idempotent within a tick: calling it a second time returns the same events
    // rather than recomputing. Recomputing would find an empty contact buffer and
    // report every touching pair as having separated, so a stray second call - a
    // debug print, a second subsystem wanting the list - would otherwise produce an
    // Exit for everything in the scene, every frame.
    const std::vector<PhysicsContactEvent>& collect_contact_events();

private:
    // Written from Jolt's worker threads during Update(), so every access is under
    // contact_mutex. Jolt forbids taking *its* locks in a contact callback; this is
    // our own mutex and is never held while calling back into Jolt.
    struct RawContact {
        uint32_t a = 0, b = 0;
        Vector3 point, normal;
        float approach_speed = 0.0f;
    };
    mutable std::mutex contact_mutex;
    std::vector<RawContact> frame_contacts;

    // Body pairs that were touching at the end of the previous tick, keyed by the
    // two ids packed into one integer. Enter and Exit are derived by differencing
    // this against the current frame rather than from OnContactRemoved, which fires
    // per sub-shape and would report an Exit while other contact points still touch.
    std::unordered_set<uint64_t> touching_pairs;
    std::vector<PhysicsContactEvent> contact_events;
    bool contacts_resolved_this_tick = false;

    struct BodyRecord {
        Actor* actor = nullptr;
        bool is_sensor = false;
    };
    mutable std::mutex registry_mutex;
    std::unordered_map<uint32_t, BodyRecord> body_registry;

    friend class LithiumContactListener;

public:

private:
    PhysicsEngine() = default;
    ~PhysicsEngine() = default;

    PhysicsEngine(const PhysicsEngine&) = delete;
    PhysicsEngine& operator=(const PhysicsEngine&) = delete;

    std::unique_ptr<JPH::PhysicsSystem> physics_system;
    std::unique_ptr<JPH::JobSystemThreadPool> job_system;
    std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator;
    
    // Broad phase layers and object layer pairs will be allocated in the cpp
    void* bp_layer_interface = nullptr;
    void* obj_bp_filter = nullptr;
    void* obj_layer_pair_filter = nullptr;

    bool is_initialized = false;
};
