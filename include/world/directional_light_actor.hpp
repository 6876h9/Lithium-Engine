#pragma once

#include "world/actor.hpp"
#include "world/static_mesh_component.hpp"
#include "world/light_components.hpp"
#include <iostream>

class DirectionalLightActor : public Actor {
public:
    float light_intensity = 1.0f;
    bool enable_msaa = true;
    bool enable_ray_tracing = true;
    bool enable_taa = true;
    float upscaling_scale = 1.0f;
    bool enable_tesla = false;
    bool enable_embree = false;
    bool enable_3d_clouds = false;
    // Background controls, surfaced in the sun's Details panel because the sky is a
    // property of the scene's key light rather than of any individual object.
    int sky_mode = 0;   // 0 = Environment HDRI (default), 1 = Procedural Sky, 2 = Void Colour
    Vector3 void_color = { 0.015f, 0.02f, 0.045f };

    DirectionalLightActor(const std::string& name) : Actor(name) {
        auto mesh = create_component<StaticMeshComponent>("Sphere");
        set_root_component(mesh);
        
        light_comp = create_component<DirectionalLightComponent>("SunLight");
        
        actor_color = { 1.0f, 0.9f, 0.4f }; // Default sun color (yellow-ish)
        // Shrink the representation so it doesn't take up too much space
        get_actor_transform().scale = { 0.4f, 0.4f, 0.4f };
        // Set an initial rotation so it looks down diagonally
        get_actor_transform().rotation = { -0.8f, -0.6f, 0.0f };
    }

    virtual void begin_play() override {}
    virtual void tick(float delta_time) override {
        if (light_comp) {
            light_comp->enable_ray_tracing = enable_ray_tracing;
            light_comp->enable_embree = enable_embree;
            light_comp->enable_3d_clouds = enable_3d_clouds;
        }
    }
    
    DirectionalLightComponent* light_comp = nullptr;
};
