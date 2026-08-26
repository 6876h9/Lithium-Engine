#pragma once

#include "core/math.hpp"
#include "world/collision_event.hpp"
#include <string>

class Actor;

class ActorComponent {
public:
    ActorComponent(Actor* owner, const std::string& name);
    virtual ~ActorComponent() = default;

    virtual void begin_play() {}
    virtual void tick(float delta_time) {}

    // Physics contact events, delivered once per body pair per frame from the scene
    // thread (never from a Jolt worker), so an override may freely touch the scene.
    // Enter fires on the frame contact begins, Stay every frame it continues, Exit
    // when it ends - including when the other actor was destroyed, in which case
    // info.other is null.
    virtual void on_collision_enter(const CollisionInfo& info) {}
    virtual void on_collision_stay(const CollisionInfo& info) {}
    virtual void on_collision_exit(const CollisionInfo& info) {}

    // Same lifecycle, but for a pair where either side is marked a trigger. A
    // trigger reports overlap and produces no collision response, so there is no
    // contact point or impact speed to report.
    virtual void on_trigger_enter(Actor* other) {}
    virtual void on_trigger_stay(Actor* other) {}
    virtual void on_trigger_exit(Actor* other) {}

    // UI events from a UICanvasComponent on the same actor, delivered once per
    // frame from the thread that owns the UI frame. A click fires when the pointer
    // is released over the widget it went down on; value_changed fires while a
    // slider is dragged, a checkbox is toggled, or a text field is typed into.
    virtual void on_ui_click(const std::string& widget_name) {}
    virtual void on_ui_value_changed(const std::string& widget_name, float value) {}

    Actor* get_owner() const { return owner; }
    const std::string& get_name() const { return name; }

protected:
    Actor* owner;
    std::string name;
};

class SceneComponent : public ActorComponent {
public:
    SceneComponent(Actor* owner, const std::string& name);
    virtual ~SceneComponent() = default;

    Transform transform;
};
