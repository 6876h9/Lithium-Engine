#pragma once

#include "core/math.hpp"
#include "world/component.hpp"
#include "world/material.hpp"
#include <vector>
#include <string>
#include <memory>

class Actor {
public:
    Actor(const std::string& name);
    virtual ~Actor();

    virtual void begin_play();
    virtual void tick(float delta_time);

    // Fan a physics event out to every component on this actor.
    void dispatch_collision_enter(const CollisionInfo& info);
    void dispatch_collision_stay(const CollisionInfo& info);
    void dispatch_collision_exit(const CollisionInfo& info);
    void dispatch_trigger_enter(Actor* other);
    void dispatch_trigger_stay(Actor* other);
    void dispatch_trigger_exit(Actor* other);
    void dispatch_ui_click(const std::string& widget_name);
    void dispatch_ui_value_changed(const std::string& widget_name, float value);

    template<typename T, typename... Args>
    T* create_component(const std::string& name, Args&&... args) {
        auto comp = std::make_unique<T>(this, name, std::forward<Args>(args)...);
        T* raw_ptr = comp.get();
        components.push_back(std::move(comp));
        return raw_ptr;
    }
    
    template<typename T>
    T* get_component() const {
        for (const auto& comp : components) {
            if (T* typed = dynamic_cast<T*>(comp.get())) {
                return typed;
            }
        }
        return nullptr;
    }

    void set_root_component(SceneComponent* component);
    SceneComponent* get_root_component() const { return root_component; }

    void remove_component(ActorComponent* comp) {
        for (auto it = components.begin(); it != components.end(); ++it) {
            if (it->get() == comp) {
                components.erase(it);
                break;
            }
        }
    }

    // Detach a component and hand back ownership, instead of destroying it the way
    // remove_component does. The undo stack needs the original object preserved: a
    // removal that destroyed it could only ever restore a default-constructed stand-in,
    // silently discarding whatever the user had configured. Returns null if the
    // component does not belong to this actor.
    std::unique_ptr<ActorComponent> release_component(ActorComponent* comp) {
        for (auto it = components.begin(); it != components.end(); ++it) {
            if (it->get() == comp) {
                std::unique_ptr<ActorComponent> detached = std::move(*it);
                components.erase(it);
                // A detached root would leave a dangling root_component behind.
                if (root_component == static_cast<void*>(detached.get())) {
                    root_component = nullptr;
                }
                return detached;
            }
        }
        return nullptr;
    }

    // Re-attach a previously released component at its original position, so an undone
    // removal restores list order rather than appending to the end. An out-of-range or
    // negative index appends.
    void adopt_component(std::unique_ptr<ActorComponent> comp, int index = -1) {
        if (!comp) return;
        if (index < 0 || static_cast<size_t>(index) >= components.size()) {
            components.push_back(std::move(comp));
        } else {
            components.insert(components.begin() + index, std::move(comp));
        }
    }

    // Position of a component in the list, or -1 if it is not this actor's.
    int index_of_component(const ActorComponent* comp) const {
        for (size_t i = 0; i < components.size(); ++i) {
            if (components[i].get() == comp) return static_cast<int>(i);
        }
        return -1;
    }

    const std::vector<std::unique_ptr<ActorComponent>>& get_components() const { return components; }

    Transform& get_actor_transform();
    const std::string& get_name() const { return name; }

    // Outliner folder this actor is filed under, as a '/'-separated path
    // ("Lighting/Interior"). Empty means the root. Purely an editor-side
    // organisation aid - it has no effect on transforms, parenting or gameplay,
    // which is exactly why it is a string here rather than real hierarchy.
    const std::string& get_folder_path() const { return folder_path; }
    void set_folder_path(const std::string& path) { folder_path = path; }
    void set_name(const std::string& new_name) { name = new_name; }

    // Editor editable properties
    bool is_invisible = false;
    Vector3 actor_color = {1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.4f; // moderately glossy default so specular highlights and IBL reflections are visible out of the box
    float clearcoat = 0.0f;
    float clearcoat_roughness = 0.1f;
    float sheen = 0.0f;
    float subsurface = 0.0f;
    // Scales the normal map's perturbation. 0 is the bare geometric surface, 1 is
    // the map as authored, and above that exaggerates it. Only has an effect on a
    // mesh that actually resolved a normal map.
    float normal_strength = 1.0f;
    // Hue the emission takes. The scalar `emissive` above is its intensity, so a
    // grey lamp housing with a warm bulb is these two together.
    Vector3 emission_color = { 1.0f, 1.0f, 1.0f };
    // How far the dielectric specular highlight takes the albedo's hue instead of
    // staying white. Irrelevant once metallic reaches 1.
    float specular_tint = 0.0f;
    // Self-illumination. Added directly to the surface's outgoing radiance, so it
    // stays bright regardless of scene lighting and blooms like a real light source.
    float emissive = 0.0f;

    // Static lighting. Marking an actor static declares its lighting will not change
    // at runtime, which is what makes it eligible for an offline bake.
    bool is_static = false;
    bool has_baked_lighting = false;
    // Baked incoming irradiance. Applied multiplied by albedo, so it acts as
    // precomputed diffuse bounce rather than as a flat additive glow.
    float baked_irradiance = 0.0f;

    // --- Motion, drivable from scripts -------------------------------------
    // Linear velocity in world units per second, and angular velocity in radians per
    // second. Integrated by the engine while the game is playing.
    //
    // If the actor has a Jolt rigid body, PhysicsAttribute owns its motion and this is
    // applied to that body instead; for everything else this gives scripts real
    // movement without requiring a physics body, which is what most gameplay wants.
    Vector3 velocity = { 0.0f, 0.0f, 0.0f };
    Vector3 angular_velocity = { 0.0f, 0.0f, 0.0f };
    std::string shape_type = "Custom";
    std::string mesh_path = "";
    
    // --- Prefab linkage ------------------------------------------------------
    // Path of the prefab this actor was instantiated from, or empty for an actor
    // authored directly in the scene. A linked instance stores only the properties
    // that differ from the prefab, so editing the prefab updates every instance that
    // has not overridden the thing being edited.
    std::string prefab_source = "";

    std::string material_path = "";
    std::shared_ptr<Material> assigned_material = nullptr;

private:
    std::string name;
    std::string folder_path;
    SceneComponent* root_component = nullptr;
    std::vector<std::unique_ptr<ActorComponent>> components;
};
