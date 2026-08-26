#pragma once

#include "world/component.hpp"
#include "world/actor.hpp"

class LightComponent : public ActorComponent {
public:
    LightComponent(Actor* owner, const std::string& name) : ActorComponent(owner, name) {}
    
    Vector3 color = { 1.0f, 1.0f, 1.0f };
    float intensity = 1.0f;
    bool enable_msaa = true;
    bool enable_taa = true;
    float upscaling_scale = 1.0f;
    bool enable_ray_tracing = true;
    bool enable_embree = false;
    bool enable_3d_clouds = false;
    // Scene background mode: 0 = environment HDRI, 1 = procedural sky/clouds,
    // 2 = flat void colour.
    int sky_mode = 0;   // default: environment HDRI
    Vector3 void_color = { 0.015f, 0.02f, 0.045f };
};

class DirectionalLightComponent : public LightComponent {
public:
    DirectionalLightComponent(Actor* owner, const std::string& name) : LightComponent(owner, name) {}

    Vector3 get_direction() const {
        if (!owner) return {0, -1, 0};
        Vector3 rot = owner->get_actor_transform().rotation;
        Matrix4x4 rotMat = Matrix4x4::rotationZ(rot.z) * Matrix4x4::rotationX(rot.x) * Matrix4x4::rotationY(rot.y);
        Vector3 forward = {0, 0, -1};
        return (rotMat * forward).normalized();
    }
};

class PointLightComponent : public LightComponent {
public:
    float radius = 10.0f;
    PointLightComponent(Actor* owner, const std::string& name) : LightComponent(owner, name) {}
};

class SpotLightComponent : public LightComponent {
public:
    float inner_angle = 12.5f;
    float outer_angle = 17.5f;
    SpotLightComponent(Actor* owner, const std::string& name) : LightComponent(owner, name) {}

    Vector3 get_direction() const {
        if (!owner) return {0, -1, 0};
        Vector3 rot = owner->get_actor_transform().rotation;
        Matrix4x4 rotMat = Matrix4x4::rotationZ(rot.z) * Matrix4x4::rotationX(rot.x) * Matrix4x4::rotationY(rot.y);
        Vector3 forward = {0, 0, -1};
        return (rotMat * forward).normalized();
    }
};

class AreaLightComponent : public LightComponent {
public:
    AreaLightComponent(Actor* owner, const std::string& name) : LightComponent(owner, name) {}
};

class SkyLightComponent : public LightComponent {
public:
    SkyLightComponent(Actor* owner, const std::string& name) : LightComponent(owner, name) {}
};
