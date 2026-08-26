#include "world/actor.hpp"

Actor::Actor(const std::string& name) : name(name) {}

Actor::~Actor() {}

void Actor::begin_play() {
    for (auto& comp : components) {
        comp->begin_play();
    }
}

void Actor::tick(float delta_time) {
    for (auto& comp : components) {
        comp->tick(delta_time);
    }
}

void Actor::dispatch_collision_enter(const CollisionInfo& info) {
    for (auto& comp : components) comp->on_collision_enter(info);
}
void Actor::dispatch_collision_stay(const CollisionInfo& info) {
    for (auto& comp : components) comp->on_collision_stay(info);
}
void Actor::dispatch_collision_exit(const CollisionInfo& info) {
    for (auto& comp : components) comp->on_collision_exit(info);
}
void Actor::dispatch_trigger_enter(Actor* other) {
    for (auto& comp : components) comp->on_trigger_enter(other);
}
void Actor::dispatch_trigger_stay(Actor* other) {
    for (auto& comp : components) comp->on_trigger_stay(other);
}
void Actor::dispatch_trigger_exit(Actor* other) {
    for (auto& comp : components) comp->on_trigger_exit(other);
}
void Actor::dispatch_ui_click(const std::string& widget_name) {
    for (auto& comp : components) comp->on_ui_click(widget_name);
}
void Actor::dispatch_ui_value_changed(const std::string& widget_name, float value) {
    for (auto& comp : components) comp->on_ui_value_changed(widget_name, value);
}

void Actor::set_root_component(SceneComponent* component) {
    root_component = component;
}

Transform& Actor::get_actor_transform() {
    static Transform default_transform;
    if (root_component) {
        return root_component->transform;
    }
    return default_transform;
}
