#pragma once
#include "world/actor.hpp"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

class Command {
public:
    virtual ~Command() = default;
    virtual void undo() = 0;
    virtual void redo() = 0;
};

class TransformCommand : public Command {
public:
    struct ActorTransformState {
        Actor* actor;
        Transform transform;
    };

    TransformCommand(const std::vector<ActorTransformState>& old_states, 
                     const std::vector<ActorTransformState>& new_states)
        : old_states(old_states), new_states(new_states) {}

    void undo() override {
        for (const auto& state : old_states) {
            if (state.actor) {
                state.actor->get_actor_transform() = state.transform;
            }
        }
    }

    void redo() override {
        for (const auto& state : new_states) {
            if (state.actor) {
                state.actor->get_actor_transform() = state.transform;
            }
        }
    }

private:
    std::vector<ActorTransformState> old_states;
    std::vector<ActorTransformState> new_states; // For potential redo
};

// Everything the Details panel can edit on one actor, captured as a plain value.
// Property edits are recorded as a before/after pair of these rather than as one
// command class per field - there are a dozen fields and they all behave the same.
struct ActorPropertyState {
    Actor* actor = nullptr;
    std::string name;
    Transform transform;
    Vector3 color = {1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.4f;
    float clearcoat = 0.0f;
    float clearcoat_roughness = 0.1f;
    float sheen = 0.0f;
    float subsurface = 0.0f;
    float emissive = 0.0f;
    bool is_invisible = false;
    bool is_static = false;

    static ActorPropertyState capture(Actor* a) {
        ActorPropertyState s;
        s.actor = a;
        if (!a) return s;
        s.name = a->get_name();
        s.transform = a->get_actor_transform();
        s.color = a->actor_color;
        s.metallic = a->metallic;
        s.roughness = a->roughness;
        s.clearcoat = a->clearcoat;
        s.clearcoat_roughness = a->clearcoat_roughness;
        s.sheen = a->sheen;
        s.subsurface = a->subsurface;
        s.emissive = a->emissive;
        s.is_invisible = a->is_invisible;
        s.is_static = a->is_static;
        return s;
    }

    void apply() const {
        if (!actor) return;
        actor->set_name(name);
        actor->get_actor_transform() = transform;
        actor->actor_color = color;
        actor->metallic = metallic;
        actor->roughness = roughness;
        actor->clearcoat = clearcoat;
        actor->clearcoat_roughness = clearcoat_roughness;
        actor->sheen = sheen;
        actor->subsurface = subsurface;
        actor->emissive = emissive;
        actor->is_invisible = is_invisible;
        actor->is_static = is_static;
    }

    // True if anything a user could have edited actually differs, so a click that
    // merely focused a field does not push an empty entry onto the undo stack.
    bool differs_from(const ActorPropertyState& o) const {
        auto vec_eq = [](const Vector3& a, const Vector3& b) {
            return a.x == b.x && a.y == b.y && a.z == b.z;
        };
        return name != o.name ||
               transform.position.x != o.transform.position.x ||
               transform.position.y != o.transform.position.y ||
               transform.position.z != o.transform.position.z ||
               !vec_eq(transform.rotation, o.transform.rotation) ||
               !vec_eq(transform.scale, o.transform.scale) ||
               !vec_eq(color, o.color) ||
               metallic != o.metallic || roughness != o.roughness ||
               clearcoat != o.clearcoat || clearcoat_roughness != o.clearcoat_roughness ||
               sheen != o.sheen || subsurface != o.subsurface || emissive != o.emissive ||
               is_invisible != o.is_invisible || is_static != o.is_static;
    }
};

// A Details-panel edit, covering every field at once.
class PropertyCommand : public Command {
public:
    PropertyCommand(std::vector<ActorPropertyState> before, std::vector<ActorPropertyState> after)
        : before_(std::move(before)), after_(std::move(after)) {}

    void undo() override { for (const auto& s : before_) s.apply(); }
    void redo() override { for (const auto& s : after_) s.apply(); }

private:
    std::vector<ActorPropertyState> before_;
    std::vector<ActorPropertyState> after_;
};

// Actors entering or leaving the scene: spawn, duplicate and delete.
//
// Entries hold shared_ptr, which is the whole point - an actor removed from the scene
// list would otherwise be destroyed immediately, leaving nothing for undo to put back.
// Holding a strong reference here keeps deleted actors alive for exactly as long as
// they sit in the undo stack.
class SceneMutationCommand : public Command {
public:
    struct Entry {
        size_t index = 0;                  // position it occupied in the scene list
        std::shared_ptr<Actor> actor;
    };

    // present_after_redo: true for a spawn (redo puts them in), false for a delete.
    SceneMutationCommand(std::vector<std::shared_ptr<Actor>>* scene,
                         std::vector<Entry> entries,
                         bool present_after_redo)
        : scene_(scene), entries_(std::move(entries)), present_after_redo_(present_after_redo) {
        // Sorted by index so re-insertion walks low to high and each stored index
        // still refers to the right slot once the earlier ones are back in place.
        std::sort(entries_.begin(), entries_.end(),
                  [](const Entry& a, const Entry& b) { return a.index < b.index; });
    }

    void undo() override { set_present(!present_after_redo_); }
    void redo() override { set_present(present_after_redo_); }

private:
    void set_present(bool present) {
        if (!scene_) return;
        if (present) {
            for (const auto& e : entries_) {
                if (!e.actor) continue;
                // Already there (a redo running twice) - never insert a duplicate.
                if (std::find(scene_->begin(), scene_->end(), e.actor) != scene_->end()) continue;
                size_t at = std::min(e.index, scene_->size());
                scene_->insert(scene_->begin() + static_cast<long>(at), e.actor);
            }
        } else {
            for (const auto& e : entries_) {
                auto it = std::find(scene_->begin(), scene_->end(), e.actor);
                if (it != scene_->end()) scene_->erase(it);
            }
        }
    }

    std::vector<std::shared_ptr<Actor>>* scene_ = nullptr;
    std::vector<Entry> entries_;
    bool present_after_redo_ = true;
};
