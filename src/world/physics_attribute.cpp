#include "world/physics_attribute.hpp"
#include "physics/physics_engine.hpp"
#include "world/static_mesh_component.hpp"
#include "core/mesh_resource.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <iostream>

namespace {

// The mesh geometry backing an actor, whether it came from an imported .mesh
// resource or was generated in place (the editor primitives). Returns false when a
// resource exists but has not finished streaming, which is a "try again later"
// rather than an error.
bool get_owner_geometry(Actor* owner, const std::vector<Vertex>** out_vertices,
                        const std::vector<unsigned int>** out_indices, bool& out_still_loading) {
    out_still_loading = false;
    if (!owner) return false;

    auto* mesh = owner->get_component<StaticMeshComponent>();
    if (!mesh) return false;

    if (auto resource = mesh->get_mesh_resource()) {
        // LoadedCPU is enough: the collider only needs the vertex data in main
        // memory, not the GPU upload.
        ResourceState state = resource->get_state();
        if (state != ResourceState::LoadedCPU && state != ResourceState::LoadedGPU) {
            out_still_loading = (state != ResourceState::Failed);
            return false;
        }
        *out_vertices = &resource->get_cpu_vertices();
        *out_indices = &resource->get_cpu_indices();
    } else {
        *out_vertices = &mesh->get_vertices();
        *out_indices = &mesh->get_indices();
    }

    return !(*out_vertices)->empty() && !(*out_indices)->empty();
}

JPH::Vec3 scaled(const Vector3& v, const Vector3& scale) {
    return JPH::Vec3(v.x * scale.x, v.y * scale.y, v.z * scale.z);
}

} // namespace

const char* PhysicsAttribute::collider_type_name(int type) {
    switch (type) {
        case Collider_Box:        return "Box";
        case Collider_Sphere:     return "Sphere";
        case Collider_Capsule:    return "Capsule";
        case Collider_Cylinder:   return "Cylinder";
        case Collider_ConvexHull: return "Convex Hull (mesh)";
        case Collider_Mesh:       return "Triangle Mesh (static)";
        default:                  return "Unknown";
    }
}

PhysicsAttribute::PhysicsAttribute(Actor* owner, const std::string& name)
    : ActorComponent(owner, name) {}

PhysicsAttribute::~PhysicsAttribute() {
    destroy_body();
}

void PhysicsAttribute::destroy_body() {
    if (body_id == JPH::BodyID::cInvalidBodyID) return;
    PhysicsEngine::get_instance().unregister_body(body_id);
    JPH::BodyInterface* bi = PhysicsEngine::get_instance().get_body_interface();
    if (bi) {
        bi->RemoveBody(JPH::BodyID(body_id));
        bi->DestroyBody(JPH::BodyID(body_id));
    }
    body_id = JPH::BodyID::cInvalidBodyID;
}

void PhysicsAttribute::begin_play() {
    // Rebuild from scratch: Play can be pressed more than once, and the previous
    // run's body still holds the shape and the pose it ended in.
    destroy_body();
    status.clear();
    creation_pending = !try_create_body();
}

bool PhysicsAttribute::try_create_body() {
    JPH::BodyInterface* bi = PhysicsEngine::get_instance().get_body_interface();
    if (!bi) {
        status = "Physics engine not initialised.";
        return true; // Nothing to wait for; retrying will not help.
    }

    Transform& trans = owner->get_actor_transform();
    JPH::RVec3 position(trans.position.x, trans.position.y, trans.position.z);
    JPH::Quat rotation = JPH::Quat::sEulerAngles(JPH::Vec3(trans.rotation.x, trans.rotation.y, trans.rotation.z));

    // A collider cannot be mirrored or zero-sized; clamp before anything is built
    // from it, or Jolt asserts deep inside shape construction.
    Vector3 scale = {
        std::max(0.001f, std::abs(trans.scale.x)),
        std::max(0.001f, std::abs(trans.scale.y)),
        std::max(0.001f, std::abs(trans.scale.z))
    };
    // The round shapes take a single radius, so a non-uniform scale has to collapse
    // to one number. The largest axis is the safe choice: it over-covers rather than
    // leaving geometry poking out of its own collider.
    float uniform_scale = std::max(scale.x, std::max(scale.y, scale.z));

    bool wants_static = (mass <= 0.0f);
    JPH::RefConst<JPH::Shape> shape;

    switch (collider_type) {
        case Collider_Sphere:
            shape = new JPH::SphereShape(std::max(0.001f, sphere_radius * uniform_scale));
            break;

        case Collider_Capsule:
            shape = new JPH::CapsuleShape(std::max(0.001f, capsule_half_height * scale.y),
                                          std::max(0.001f, capsule_radius * std::max(scale.x, scale.z)));
            break;

        case Collider_Cylinder:
            shape = new JPH::CylinderShape(std::max(0.001f, cylinder_half_height * scale.y),
                                           std::max(0.001f, cylinder_radius * std::max(scale.x, scale.z)));
            break;

        case Collider_ConvexHull:
        case Collider_Mesh: {
            const std::vector<Vertex>* vertices = nullptr;
            const std::vector<unsigned int>* indices = nullptr;
            bool still_loading = false;
            if (!get_owner_geometry(owner, &vertices, &indices, still_loading)) {
                if (still_loading) {
                    status = "Waiting for mesh to finish loading...";
                    return false;
                }
                status = std::string(collider_type_name(collider_type)) +
                         " needs a mesh on this actor; falling back to Box.";
                std::cerr << "[Physics] " << owner->get_name() << ": " << status << std::endl;
                shape = new JPH::BoxShape(scaled(box_half_extents, scale));
                break;
            }

            if (collider_type == Collider_ConvexHull) {
                JPH::Array<JPH::Vec3> points;
                points.reserve(vertices->size());
                for (const Vertex& v : *vertices) points.push_back(scaled(v.position, scale));

                JPH::ConvexHullShapeSettings settings(points, JPH::cDefaultConvexRadius);
                JPH::ShapeSettings::ShapeResult result = settings.Create();
                if (result.HasError()) {
                    status = "Convex hull failed (" + std::string(result.GetError()) + "); falling back to Box.";
                    std::cerr << "[Physics] " << owner->get_name() << ": " << status << std::endl;
                    shape = new JPH::BoxShape(scaled(box_half_extents, scale));
                } else {
                    shape = result.Get();
                }
            } else {
                // A triangle mesh is one-sided, hollow and concave; Jolt only allows
                // it on a static body. Forcing it here beats letting the assert fire
                // inside body creation with no explanation.
                if (!wants_static) {
                    wants_static = true;
                    status = "Triangle mesh colliders must be static; this body will not move.";
                }

                JPH::TriangleList triangles;
                triangles.reserve(indices->size() / 3);
                for (size_t i = 0; i + 2 < indices->size(); i += 3) {
                    unsigned int i0 = (*indices)[i], i1 = (*indices)[i + 1], i2 = (*indices)[i + 2];
                    if (i0 >= vertices->size() || i1 >= vertices->size() || i2 >= vertices->size()) continue;
                    triangles.push_back(JPH::Triangle(scaled((*vertices)[i0].position, scale),
                                                      scaled((*vertices)[i1].position, scale),
                                                      scaled((*vertices)[i2].position, scale)));
                }

                JPH::MeshShapeSettings settings(triangles);
                JPH::ShapeSettings::ShapeResult result = settings.Create();
                if (result.HasError()) {
                    status = "Triangle mesh failed (" + std::string(result.GetError()) + "); falling back to Box.";
                    std::cerr << "[Physics] " << owner->get_name() << ": " << status << std::endl;
                    shape = new JPH::BoxShape(scaled(box_half_extents, scale));
                } else {
                    shape = result.Get();
                }
            }
            break;
        }

        case Collider_Box:
        default:
            shape = new JPH::BoxShape(scaled(box_half_extents, scale));
            break;
    }

    // The object layer packs the gameplay layer together with whether the body
    // moves; Jolt needs the second half to skip static-versus-static pairs.
    JPH::ObjectLayer layer = PhysicsEngine::make_object_layer(collision_layer, !wants_static);
    JPH::EMotionType motion_type = wants_static ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic;

    JPH::BodyCreationSettings settings(shape, position, rotation, motion_type, layer);

    if (!wants_static) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = mass;
    }

    settings.mFriction = friction;
    settings.mRestitution = restitution;
    settings.mGravityFactor = simulate_gravity ? 1.0f : 0.0f;

    settings.mIsSensor = is_trigger;

    JPH::BodyID id = bi->CreateAndAddBody(settings, JPH::EActivation::Activate);
    body_id = id.GetIndexAndSequenceNumber();
    // Contact callbacks only carry BodyIDs, so the engine needs this mapping to name
    // the actor a contact belongs to.
    PhysicsEngine::get_instance().register_body(body_id, owner, is_trigger);
    return true;
}

void PhysicsAttribute::tick(float delta_time) {
    if (creation_pending) {
        creation_pending = !try_create_body();
        return;
    }

    if (body_id == JPH::BodyID::cInvalidBodyID) return;

    JPH::BodyInterface* bi = PhysicsEngine::get_instance().get_body_interface();
    if (!bi) return;

    // A static body never moves, so reading it back every frame would only
    // overwrite the authored transform with a rounded copy of itself.
    if (!bi->IsActive(JPH::BodyID(body_id)) && mass <= 0.0f) return;

    JPH::RVec3 pos = bi->GetPosition(JPH::BodyID(body_id));
    JPH::Quat rot = bi->GetRotation(JPH::BodyID(body_id));

    Transform& trans = owner->get_actor_transform();
    trans.position = DVector3{ static_cast<double>(pos.GetX()), static_cast<double>(pos.GetY()), static_cast<double>(pos.GetZ()) };

    JPH::Vec3 euler = rot.GetEulerAngles();
    trans.rotation = { euler.GetX(), euler.GetY(), euler.GetZ() };
}
