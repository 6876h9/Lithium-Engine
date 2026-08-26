#pragma once

#include "world/component.hpp"

class CameraComponent : public SceneComponent {
public:
    CameraComponent(Actor* owner, const std::string& name);
    virtual ~CameraComponent() = default;

    float fov = 45.0f;
    float near_plane = 0.1f;
    float far_plane = 1000.0f;
    bool is_active = true;
};
