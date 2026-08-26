#include "world/component.hpp"
#include "world/actor.hpp"

ActorComponent::ActorComponent(Actor* owner, const std::string& name)
    : owner(owner), name(name) {}

SceneComponent::SceneComponent(Actor* owner, const std::string& name)
    : ActorComponent(owner, name) {}
