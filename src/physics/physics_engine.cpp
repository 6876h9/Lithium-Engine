#include "physics/physics_engine.hpp"
#include <string>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/Body.h>
#include <iostream>
#include <cstdarg>
#include <cmath>
#include <algorithm>

using namespace JPH;

// Callback for traces
static void TraceImpl(const char* inFMT, ...) {
    va_list list;
    va_start(list, inFMT);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), inFMT, list);
    va_end(list);
    std::cout << buffer << std::endl;
}

#ifdef JPH_ENABLE_ASSERTS
// Callback for asserts
static bool AssertFailedImpl(const char* inExpression, const char* inMessage, const char* inFile, uint32_t inLine) {
    std::cout << inFile << ":" << inLine << ": (" << inExpression << ") " << (inMessage ? inMessage : "") << std::endl;
    return true; // Breakpoint
}
#endif // JPH_ENABLE_ASSERTS

// Layer definitions
//
// Two broad phase layers - things that move and things that do not - and one object
// layer per (gameplay layer, moving) pair. Splitting the broad phase that way is
// what lets Jolt skip the enormous number of static-versus-static pairs a level
// geometry mesh would otherwise generate.
namespace Layers {
    static constexpr BroadPhaseLayer::Type BP_NON_MOVING = 0;
    static constexpr BroadPhaseLayer::Type BP_MOVING = 1;
    static constexpr uint32_t NUM_BROAD_PHASE_LAYERS = 2;
    // Two object layers per gameplay layer: the static half and the moving half.
    static constexpr ObjectLayer NUM_OBJECT_LAYERS =
        static_cast<ObjectLayer>(PhysicsEngine::kLayerCount * 2);
};

// The collision matrix and the layer names.
//
// File-scope rather than members because the filter objects below are plain Jolt
// interfaces with no route back to the engine instance, and because the editor
// edits these before physics has been initialised at all.
struct LayerSettings {
    // Bit j of mask[i] means layer i collides with layer j. Kept symmetric by the
    // setter; a one-directional collision is not something a solver can express.
    uint32_t mask[PhysicsEngine::kLayerCount];
    std::string names[PhysicsEngine::kLayerCount];

    LayerSettings() { reset(); }

    void reset() {
        for (int i = 0; i < PhysicsEngine::kLayerCount; ++i) {
            mask[i] = 0xFFFFFFFFu;
            names[i] = (i == 0) ? "Default" : ("Layer " + std::to_string(i));
        }
    }
};

LayerSettings& layer_settings() {
    // Function-local static: this is read from Jolt's filters, which can run during
    // static initialisation of other translation units.
    static LayerSettings settings;
    return settings;
}

class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface {
public:
    virtual uint32_t GetNumBroadPhaseLayers() const override { return Layers::NUM_BROAD_PHASE_LAYERS; }
    virtual BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer inLayer) const override {
        // The low bit of the object layer is the moving flag, so the broad phase
        // layer falls straight out of it without a table.
        return BroadPhaseLayer(PhysicsEngine::object_layer_is_moving(inLayer)
                                   ? Layers::BP_MOVING
                                   : Layers::BP_NON_MOVING);
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    virtual const char* GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override {
        switch ((BroadPhaseLayer::Type)inLayer) {
        case Layers::BP_NON_MOVING: return "NON_MOVING";
        case Layers::BP_MOVING: return "MOVING";
        default: JPH_ASSERT(false); return "INVALID";
        }
    }
#endif // JPH_EXTERNAL_PROFILE || JPH_PROFILE_ENABLED
};

class ObjectVsBroadPhaseLayerFilterImpl : public ObjectVsBroadPhaseLayerFilter {
public:
    virtual bool ShouldCollide(ObjectLayer inLayer1, BroadPhaseLayer inLayer2) const override {
        // A static body only ever needs testing against the moving half of the
        // world. A moving one has to be tested against both.
        if (!PhysicsEngine::object_layer_is_moving(inLayer1)) {
            return inLayer2 == BroadPhaseLayer(Layers::BP_MOVING);
        }
        return true;
    }
};

class ObjectLayerPairFilterImpl : public ObjectLayerPairFilter {
public:
    virtual bool ShouldCollide(ObjectLayer inObject1, ObjectLayer inObject2) const override {
        // Two static bodies can never touch in a way the solver would act on.
        if (!PhysicsEngine::object_layer_is_moving(inObject1) &&
            !PhysicsEngine::object_layer_is_moving(inObject2)) {
            return false;
        }
        return PhysicsEngine::layers_should_collide(PhysicsEngine::gameplay_layer_of(inObject1),
                                                    PhysicsEngine::gameplay_layer_of(inObject2));
    }
};

// Restricts a query to a set of gameplay layers. Jolt asks per object layer, so the
// gameplay half is decoded out of it and tested against the caller's bit mask.
class MaskedObjectLayerFilter : public ObjectLayerFilter {
public:
    explicit MaskedObjectLayerFilter(uint32_t mask) : mask(mask) {}
    virtual bool ShouldCollide(ObjectLayer inLayer) const override {
        const int layer = PhysicsEngine::gameplay_layer_of(inLayer);
        if (layer < 0 || layer >= PhysicsEngine::kLayerCount) return false;
        return (mask & (1u << layer)) != 0;
    }
private:
    uint32_t mask;
};

class MyBodyActivationListener : public BodyActivationListener {
public:
    virtual void OnBodyActivated(const BodyID& inBodyID, uint64_t inBodyUserData) override {}
    virtual void OnBodyDeactivated(const BodyID& inBodyID, uint64_t inBodyUserData) override {}
};

// Records every contact Jolt reports during a step so the engine can turn them into
// gameplay events afterwards.
//
// These callbacks run on Jolt's worker threads with all bodies already locked, so
// nothing here may call back into Jolt's locking interface or into gameplay code.
// All it does is append to a buffer under our own mutex; the interpretation happens
// later on the scene thread in collect_contact_events().
class LithiumContactListener : public ContactListener {
public:
    virtual ValidateResult OnContactValidate(const Body& inBody1, const Body& inBody2, RVec3Arg inBaseOffset, const CollideShapeResult& inCollisionResult) override {
        return ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    virtual void OnContactAdded(const Body& inBody1, const Body& inBody2, const ContactManifold& inManifold, ContactSettings& ioSettings) override {
        record(inBody1, inBody2, inManifold);
    }

    virtual void OnContactPersisted(const Body& inBody1, const Body& inBody2, const ContactManifold& inManifold, ContactSettings& ioSettings) override {
        record(inBody1, inBody2, inManifold);
    }

    // Intentionally not used to drive Exit. Jolt fires this per sub-shape pair, so a
    // box pivoting on a floor loses individual contact points constantly while the
    // two are still very much touching. Exit is derived from a pair vanishing across
    // a whole frame instead - see PhysicsEngine::collect_contact_events.
    virtual void OnContactRemoved(const SubShapeIDPair& inSubShapePair) override {}

private:
    void record(const Body& body1, const Body& body2, const ContactManifold& manifold) {
        if (manifold.mRelativeContactPointsOn1.empty()) return;

        PhysicsEngine& engine = PhysicsEngine::get_instance();

        PhysicsEngine::RawContact contact;
        contact.a = body1.GetID().GetIndexAndSequenceNumber();
        contact.b = body2.GetID().GetIndexAndSequenceNumber();

        RVec3 point = manifold.GetWorldSpaceContactPointOn1(0);
        contact.point = { static_cast<float>(point.GetX()), static_cast<float>(point.GetY()), static_cast<float>(point.GetZ()) };

        Vec3 normal = manifold.mWorldSpaceNormal;
        contact.normal = { normal.GetX(), normal.GetY(), normal.GetZ() };

        // Sampled before the solver runs, so this is the speed the two were closing
        // at - the number worth scaling an impact sound or damage by. Afterwards it
        // would always read as roughly zero, because resolving the contact is
        // precisely what removes the closing velocity.
        Vec3 relative = body2.GetLinearVelocity() - body1.GetLinearVelocity();
        contact.approach_speed = std::abs(relative.Dot(normal));

        std::lock_guard<std::mutex> lock(engine.contact_mutex);
        engine.frame_contacts.push_back(contact);
    }
};

static MyBodyActivationListener body_activation_listener;
static LithiumContactListener contact_listener;

PhysicsEngine& PhysicsEngine::get_instance() {
    static PhysicsEngine instance;
    return instance;
}

bool PhysicsEngine::layers_should_collide(int layer_a, int layer_b) {
    if (layer_a < 0 || layer_a >= kLayerCount || layer_b < 0 || layer_b >= kLayerCount) return true;
    return (layer_settings().mask[layer_a] & (1u << layer_b)) != 0;
}

void PhysicsEngine::set_layers_collide(int layer_a, int layer_b, bool enabled) {
    if (layer_a < 0 || layer_a >= kLayerCount || layer_b < 0 || layer_b >= kLayerCount) return;
    // Both directions, always. The matrix the editor draws is triangular precisely
    // because the relation is symmetric.
    if (enabled) {
        layer_settings().mask[layer_a] |= (1u << layer_b);
        layer_settings().mask[layer_b] |= (1u << layer_a);
    } else {
        layer_settings().mask[layer_a] &= ~(1u << layer_b);
        layer_settings().mask[layer_b] &= ~(1u << layer_a);
    }
}

uint32_t PhysicsEngine::get_layer_mask(int layer) {
    if (layer < 0 || layer >= kLayerCount) return 0xFFFFFFFFu;
    return layer_settings().mask[layer];
}

const std::string& PhysicsEngine::get_layer_name(int layer) {
    static const std::string invalid = "Invalid";
    if (layer < 0 || layer >= kLayerCount) return invalid;
    return layer_settings().names[layer];
}

void PhysicsEngine::set_layer_name(int layer, const std::string& name) {
    if (layer < 0 || layer >= kLayerCount) return;
    layer_settings().names[layer] = name.empty() ? ("Layer " + std::to_string(layer)) : name;
}

void PhysicsEngine::reset_layers() {
    layer_settings().reset();
}

void PhysicsEngine::initialize() {
    if (is_initialized) return;

    RegisterDefaultAllocator();

    Trace = TraceImpl;
    JPH_IF_ENABLE_ASSERTS(AssertFailed = AssertFailedImpl;)

    Factory::sInstance = new Factory();
    RegisterTypes();

    temp_allocator = std::make_unique<TempAllocatorImpl>(10 * 1024 * 1024);
    job_system = std::make_unique<JobSystemThreadPool>(cMaxPhysicsJobs, cMaxPhysicsBarriers, thread::hardware_concurrency() - 1);

    const uint32_t cMaxBodies = 10240;
    const uint32_t cNumBodyMutexes = 0; // default
    const uint32_t cMaxBodyPairs = 65536;
    const uint32_t cMaxContactConstraints = 10240;

    bp_layer_interface = new BPLayerInterfaceImpl();
    obj_bp_filter = new ObjectVsBroadPhaseLayerFilterImpl();
    obj_layer_pair_filter = new ObjectLayerPairFilterImpl();

    physics_system = std::make_unique<PhysicsSystem>();
    physics_system->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints, 
                         *static_cast<BPLayerInterfaceImpl*>(bp_layer_interface), 
                         *static_cast<ObjectVsBroadPhaseLayerFilterImpl*>(obj_bp_filter), 
                         *static_cast<ObjectLayerPairFilterImpl*>(obj_layer_pair_filter));

    physics_system->SetBodyActivationListener(&body_activation_listener);
    physics_system->SetContactListener(&contact_listener);

    is_initialized = true;
}

void PhysicsEngine::cleanup() {
    if (!is_initialized) return;

    physics_system.reset();
    job_system.reset();
    temp_allocator.reset();

    delete static_cast<BPLayerInterfaceImpl*>(bp_layer_interface);
    delete static_cast<ObjectVsBroadPhaseLayerFilterImpl*>(obj_bp_filter);
    delete static_cast<ObjectLayerPairFilterImpl*>(obj_layer_pair_filter);

    {
        std::lock_guard<std::mutex> lock(contact_mutex);
        frame_contacts.clear();
    }
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        body_registry.clear();
    }
    touching_pairs.clear();
    contact_events.clear();
    contacts_resolved_this_tick = false;

    UnregisterTypes();
    delete Factory::sInstance;
    Factory::sInstance = nullptr;

    is_initialized = false;
}

void PhysicsEngine::tick(float delta_time) {
    if (!is_initialized) return;
    const int cCollisionSteps = 1;
    // Cleared before the step, so the contacts this step produces are resolved once
    // and any further request in the same frame gets that same answer.
    contacts_resolved_this_tick = false;
    physics_system->Update(delta_time, cCollisionSteps, temp_allocator.get(), job_system.get());
}

BodyInterface* PhysicsEngine::get_body_interface() {
    if (!is_initialized) return nullptr;
    return &physics_system->GetBodyInterface();
}

namespace {
// Body pairs are keyed by both ids packed into one integer, with the lower id first
// so the same pair hashes identically no matter which order Jolt reports it in.
uint64_t pair_key(uint32_t a, uint32_t b) {
    uint32_t lo = std::min(a, b), hi = std::max(a, b);
    return (static_cast<uint64_t>(lo) << 32) | hi;
}
} // namespace

void PhysicsEngine::register_body(uint32_t body_id, Actor* actor, bool is_sensor) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    body_registry[body_id] = BodyRecord{ actor, is_sensor };
}

void PhysicsEngine::unregister_body(uint32_t body_id) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    body_registry.erase(body_id);
}

Actor* PhysicsEngine::actor_for_body(uint32_t body_id) const {
    std::lock_guard<std::mutex> lock(registry_mutex);
    auto it = body_registry.find(body_id);
    return it == body_registry.end() ? nullptr : it->second.actor;
}

const std::vector<PhysicsContactEvent>& PhysicsEngine::collect_contact_events() {
    if (contacts_resolved_this_tick) return contact_events;
    contacts_resolved_this_tick = true;
    contact_events.clear();

    std::vector<RawContact> raw;
    {
        std::lock_guard<std::mutex> lock(contact_mutex);
        raw.swap(frame_contacts);
    }

    // One entry per body pair. Jolt reports a contact per sub-shape pair, so a box
    // sitting flat on a floor arrives here four times; gameplay wants one event.
    std::unordered_map<uint64_t, RawContact> current;
    current.reserve(raw.size());
    for (const RawContact& contact : raw) {
        // The deepest-closing contact wins, so approach_speed describes the hardest
        // part of the impact rather than whichever corner happened to be reported first.
        auto it = current.find(pair_key(contact.a, contact.b));
        if (it == current.end()) {
            current.emplace(pair_key(contact.a, contact.b), contact);
        } else if (contact.approach_speed > it->second.approach_speed) {
            it->second = contact;
        }
    }

    auto is_sensor_pair = [this](uint32_t a, uint32_t b) {
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto ia = body_registry.find(a);
        auto ib = body_registry.find(b);
        return (ia != body_registry.end() && ia->second.is_sensor)
            || (ib != body_registry.end() && ib->second.is_sensor);
    };

    for (const auto& entry : current) {
        const RawContact& contact = entry.second;
        PhysicsContactEvent event;
        event.phase = touching_pairs.count(entry.first) ? PhysicsContactEvent::Phase::Stay
                                                        : PhysicsContactEvent::Phase::Enter;
        event.body_a = contact.a;
        event.body_b = contact.b;
        event.point = contact.point;
        event.normal = contact.normal;
        // Only meaningful at the moment of impact; while resting it is noise.
        event.approach_speed = (event.phase == PhysicsContactEvent::Phase::Enter) ? contact.approach_speed : 0.0f;
        event.is_trigger = is_sensor_pair(contact.a, contact.b);
        contact_events.push_back(event);
    }

    // Pairs that stopped being reported. Not all of them have actually separated:
    // Jolt stops reporting contacts for a body once it falls asleep (and a static
    // sensor loses its contact point entirely), so a box resting undisturbed on the
    // floor would otherwise fire a spurious Exit the moment it settles.
    JPH::BodyInterface* bodies = is_initialized ? &physics_system->GetBodyInterface() : nullptr;
    std::unordered_set<uint64_t> still_touching;
    still_touching.reserve(current.size() + touching_pairs.size());
    for (const auto& entry : current) still_touching.insert(entry.first);

    for (uint64_t key : touching_pairs) {
        if (current.count(key)) continue;

        uint32_t a = static_cast<uint32_t>(key >> 32);
        uint32_t b = static_cast<uint32_t>(key & 0xFFFFFFFFull);

        Actor* actor_a = actor_for_body(a);
        Actor* actor_b = actor_for_body(b);

        // Both sides still exist and neither is awake: they are asleep in contact,
        // so carry the pair forward silently rather than reporting a separation.
        if (bodies && actor_a && actor_b &&
            !bodies->IsActive(JPH::BodyID(a)) && !bodies->IsActive(JPH::BodyID(b))) {
            still_touching.insert(key);
            continue;
        }

        PhysicsContactEvent event;
        event.phase = PhysicsContactEvent::Phase::Exit;
        event.body_a = a;
        event.body_b = b;
        event.is_trigger = is_sensor_pair(a, b);
        contact_events.push_back(event);
    }

    touching_pairs.swap(still_touching);
    return contact_events;
}

TempAllocator* PhysicsEngine::get_temp_allocator() {
    // Upcast lives here rather than in the header, which only forward declares both
    // types and so cannot know they are related.
    return temp_allocator.get();
}

float PhysicsEngine::get_gravity_y() const {
    if (!is_initialized) return -9.81f;
    return physics_system->GetGravity().GetY();
}

bool PhysicsEngine::raycast(double start_x, double start_y, double start_z, double dir_x, double dir_y, double dir_z, float max_distance, float& out_distance) {
    RaycastHit hit;
    if (!raycast(DVector3{ start_x, start_y, start_z },
                 Vector3{ static_cast<float>(dir_x), static_cast<float>(dir_y), static_cast<float>(dir_z) },
                 max_distance, hit)) {
        return false;
    }
    out_distance = hit.distance;
    return true;
}

namespace {

// Fills in everything a hit record carries beyond the fraction Jolt reports. The
// surface normal has to be asked of the body itself, because it depends on which
// sub-shape and which triangle of it was struck.
void fill_hit(JPH::PhysicsSystem& system, const JPH::RRayCast& ray,
              const JPH::RayCastResult& result, float max_distance,
              const PhysicsEngine& engine, RaycastHit& out_hit) {
    out_hit.distance = result.mFraction * max_distance;
    const JPH::RVec3 position = ray.GetPointOnRay(result.mFraction);
    out_hit.point = { static_cast<float>(position.GetX()),
                      static_cast<float>(position.GetY()),
                      static_cast<float>(position.GetZ()) };
    out_hit.body_id = result.mBodyID.GetIndexAndSequenceNumber();
    out_hit.actor = engine.actor_for_body(out_hit.body_id);

    JPH::BodyLockRead lock(system.GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        const JPH::Vec3 normal = body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, position);
        out_hit.normal = { normal.GetX(), normal.GetY(), normal.GetZ() };
        out_hit.layer = PhysicsEngine::gameplay_layer_of(body.GetObjectLayer());
    }
}

} // namespace

bool PhysicsEngine::raycast(const DVector3& origin, const Vector3& direction, float max_distance,
                            RaycastHit& out_hit, uint32_t layer_mask) const {
    if (!is_initialized || !physics_system) return false;

    const Vector3 unit = direction.normalized();
    if (unit.length() < 0.5f || max_distance <= 0.0f) return false;

    const JPH::RRayCast ray{
        JPH::RVec3(origin.x, origin.y, origin.z),
        JPH::Vec3(unit.x, unit.y, unit.z) * max_distance
    };

    JPH::RayCastResult result;
    const MaskedObjectLayerFilter layer_filter(layer_mask);
    if (!physics_system->GetNarrowPhaseQuery().CastRay(ray, result, {}, layer_filter)) {
        return false;
    }

    out_hit = RaycastHit{};
    fill_hit(*physics_system, ray, result, max_distance, *this, out_hit);
    return true;
}

bool PhysicsEngine::raycast_all(const DVector3& origin, const Vector3& direction, float max_distance,
                                std::vector<RaycastHit>& out_hits, uint32_t layer_mask) const {
    out_hits.clear();
    if (!is_initialized || !physics_system) return false;

    const Vector3 unit = direction.normalized();
    if (unit.length() < 0.5f || max_distance <= 0.0f) return false;

    const JPH::RRayCast ray{
        JPH::RVec3(origin.x, origin.y, origin.z),
        JPH::Vec3(unit.x, unit.y, unit.z) * max_distance
    };

    JPH::RayCastSettings settings;
    // Back faces matter here: a ray starting inside a wall should still report
    // leaving it, which is exactly what a "how much am I inside" query needs.
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;

    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const MaskedObjectLayerFilter layer_filter(layer_mask);
    physics_system->GetNarrowPhaseQuery().CastRay(ray, settings, collector, {}, layer_filter);
    if (collector.mHits.empty()) return false;

    // Nearest first: every caller that walks the list wants it in the order the ray
    // actually encountered things.
    collector.Sort();
    out_hits.reserve(collector.mHits.size());
    for (const JPH::RayCastResult& result : collector.mHits) {
        RaycastHit hit;
        fill_hit(*physics_system, ray, result, max_distance, *this, hit);
        out_hits.push_back(hit);
    }
    return true;
}

bool PhysicsEngine::sphere_cast(const DVector3& origin, const Vector3& direction, float radius,
                                float max_distance, RaycastHit& out_hit, uint32_t layer_mask) const {
    if (!is_initialized || !physics_system) return false;

    const Vector3 unit = direction.normalized();
    if (unit.length() < 0.5f || max_distance <= 0.0f || radius <= 0.0f) return false;

    JPH::SphereShape sphere(radius);
    // Jolt's shape casts assume a shape whose density has been set; a bare sphere
    // shape is fine for a query but has to be told not to be reference counted away
    // while the cast runs.
    sphere.SetEmbedded();

    const JPH::RShapeCast shape_cast(&sphere, JPH::Vec3::sReplicate(1.0f),
                                     JPH::RMat44::sTranslation(JPH::RVec3(origin.x, origin.y, origin.z)),
                                     JPH::Vec3(unit.x, unit.y, unit.z) * max_distance);

    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;

    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const MaskedObjectLayerFilter layer_filter(layer_mask);
    physics_system->GetNarrowPhaseQuery().CastShape(shape_cast, settings, JPH::RVec3::sZero(),
                                                    collector, {}, layer_filter);
    if (!collector.HadHit()) return false;

    out_hit = RaycastHit{};
    out_hit.distance = collector.mHit.mFraction * max_distance;
    // A shape cast reports the contact on the surface it hit, and its normal points
    // out of that surface once negated - Jolt gives it pointing into the shape.
    const JPH::Vec3 contact = collector.mHit.mContactPointOn2;
    out_hit.point = { contact.GetX(), contact.GetY(), contact.GetZ() };
    const JPH::Vec3 normal = -collector.mHit.mPenetrationAxis.Normalized();
    out_hit.normal = { normal.GetX(), normal.GetY(), normal.GetZ() };
    out_hit.body_id = collector.mHit.mBodyID2.GetIndexAndSequenceNumber();
    out_hit.actor = actor_for_body(out_hit.body_id);

    JPH::BodyLockRead lock(physics_system->GetBodyLockInterface(), collector.mHit.mBodyID2);
    if (lock.Succeeded()) out_hit.layer = gameplay_layer_of(lock.GetBody().GetObjectLayer());
    return true;
}

bool PhysicsEngine::overlap_sphere(const DVector3& center, float radius,
                                   std::vector<Actor*>& out_actors, uint32_t layer_mask) const {
    out_actors.clear();
    if (!is_initialized || !physics_system || radius <= 0.0f) return false;

    JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
    const MaskedObjectLayerFilter layer_filter(layer_mask);
    physics_system->GetBroadPhaseQuery().CollideSphere(
        JPH::Vec3(static_cast<float>(center.x), static_cast<float>(center.y), static_cast<float>(center.z)),
        radius, collector, {}, layer_filter);

    for (const JPH::BodyID& body : collector.mHits) {
        if (Actor* actor = actor_for_body(body.GetIndexAndSequenceNumber())) {
            // One entry per actor: a compound collider reports several bodies and an
            // explosion must not damage the same target twice.
            if (std::find(out_actors.begin(), out_actors.end(), actor) == out_actors.end()) {
                out_actors.push_back(actor);
            }
        }
    }
    return !out_actors.empty();
}
