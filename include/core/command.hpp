#pragma once
#include "world/actor.hpp"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// Undo/redo for the editor.
//
// Every mutation reachable from the UI goes through a Command, and every Command
// goes onto a CommandStack. Two rules make the difference between this and a toy
// undo system:
//
//   1. A command owns whatever state undo needs to put back. For a deletion that
//      means a strong reference to the actor - its components, its transform, its
//      place in the scene list - not just an id. Nothing else is holding it.
//   2. A continuous interaction is ONE entry. A gizmo drag or a slider scrub spans
//      hundreds of frames; recording one command per frame makes Ctrl+Z useless.
//      The editor opens a transaction on the interaction's first frame and closes
//      it on release, and everything pushed in between collapses into one entry.
class Command {
public:
    virtual ~Command() = default;
    virtual void undo() = 0;
    virtual void redo() = 0;
    // Shown in the Undo History panel. Present tense, imperative, like a commit
    // subject - the panel reads as a list of things that were done.
    virtual std::string name() const = 0;
};

// --- Transform ---------------------------------------------------------------

class TransformCommand : public Command {
public:
    struct ActorTransformState {
        Actor* actor;
        Transform transform;
    };

    TransformCommand(std::vector<ActorTransformState> old_states,
                     std::vector<ActorTransformState> new_states,
                     std::string label = "Transform")
        : old_states(std::move(old_states)), new_states(std::move(new_states)),
          label_(std::move(label)) {}

    void undo() override {
        for (const auto& state : old_states) {
            if (state.actor) state.actor->get_actor_transform() = state.transform;
        }
    }

    void redo() override {
        for (const auto& state : new_states) {
            if (state.actor) state.actor->get_actor_transform() = state.transform;
        }
    }

    std::string name() const override {
        if (old_states.size() > 1) return label_ + " " + std::to_string(old_states.size()) + " Actors";
        if (!old_states.empty() && old_states[0].actor) return label_ + " " + old_states[0].actor->get_name();
        return label_;
    }

    // True when nothing actually moved, so a click that merely armed the gizmo does
    // not push an entry that appears to do nothing when undone.
    bool is_noop() const {
        if (old_states.size() != new_states.size()) return false;
        for (size_t i = 0; i < old_states.size(); ++i) {
            const Transform& a = old_states[i].transform;
            const Transform& b = new_states[i].transform;
            if (a.position.x != b.position.x || a.position.y != b.position.y || a.position.z != b.position.z) return false;
            if (a.rotation.x != b.rotation.x || a.rotation.y != b.rotation.y || a.rotation.z != b.rotation.z) return false;
            if (a.scale.x != b.scale.x || a.scale.y != b.scale.y || a.scale.z != b.scale.z) return false;
        }
        return true;
    }

private:
    std::vector<ActorTransformState> old_states;
    std::vector<ActorTransformState> new_states;
    std::string label_;
};

// --- Properties --------------------------------------------------------------

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
    // Material assignment. Captured here too because assigning a material from the
    // content browser is a Details-panel-shaped edit: it rewrites the same PBR
    // fields, and undoing it has to restore the link as well as the numbers.
    std::string material_path;
    std::shared_ptr<Material> assigned_material;

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
        s.material_path = a->material_path;
        s.assigned_material = a->assigned_material;
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
        actor->material_path = material_path;
        actor->assigned_material = assigned_material;
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
               is_invisible != o.is_invisible || is_static != o.is_static ||
               material_path != o.material_path || assigned_material != o.assigned_material;
    }
};

// A Details-panel edit, covering every field at once.
class PropertyCommand : public Command {
public:
    PropertyCommand(std::vector<ActorPropertyState> before, std::vector<ActorPropertyState> after,
                    std::string label = "Edit")
        : before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}

    void undo() override { for (const auto& s : before_) s.apply(); }
    void redo() override { for (const auto& s : after_) s.apply(); }

    std::string name() const override {
        if (before_.size() > 1) return label_ + " " + std::to_string(before_.size()) + " Actors";
        if (!before_.empty() && before_[0].actor) return label_ + " " + before_[0].actor->get_name();
        return label_;
    }

private:
    std::vector<ActorPropertyState> before_;
    std::vector<ActorPropertyState> after_;
    std::string label_;
};

// --- Scene membership --------------------------------------------------------

// Actors entering or leaving the scene: spawn, duplicate and delete.
//
// Entries hold shared_ptr, which is the whole point - an actor removed from the scene
// list would otherwise be destroyed immediately, leaving nothing for undo to put back.
// Holding a strong reference here keeps deleted actors alive, with every component
// they own, for exactly as long as they sit in the undo stack.
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

    std::string name() const override {
        const char* verb = present_after_redo_ ? "Create " : "Delete ";
        if (entries_.size() > 1) return std::string(verb) + std::to_string(entries_.size()) + " Actors";
        if (!entries_.empty() && entries_[0].actor) return verb + entries_[0].actor->get_name();
        return present_after_redo_ ? "Create Actor" : "Delete Actor";
    }

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

// --- Components --------------------------------------------------------------

// Adding or removing a component. Like a deletion, the detached component is owned
// by the command rather than destroyed, so undo restores the real object with every
// field the user had already configured on it - not a freshly default-constructed
// replacement that silently drops their work.
class ComponentMutationCommand : public Command {
public:
    // present_after_redo: true for an add, false for a remove.
    ComponentMutationCommand(Actor* actor, ActorComponent* component, bool present_after_redo,
                             std::string type_label)
        : actor_(actor), component_(component), present_after_redo_(present_after_redo),
          type_label_(std::move(type_label)) {
        if (!present_after_redo_ && actor_ && component_) {
            // A removal is recorded before the component leaves the actor, so the
            // detach happens here, in redo()'s "already applied" position.
            detached_ = actor_->release_component(component_);
        }
    }

    void undo() override { set_present(!present_after_redo_); }
    void redo() override { set_present(present_after_redo_); }

    std::string name() const override {
        std::string verb = present_after_redo_ ? "Add " : "Remove ";
        std::string where = actor_ ? (" on " + actor_->get_name()) : std::string();
        return verb + type_label_ + where;
    }

private:
    void set_present(bool present) {
        if (!actor_) return;
        if (present) {
            if (detached_) actor_->adopt_component(std::move(detached_), detached_index_);
        } else {
            if (!detached_ && component_) {
                detached_index_ = actor_->index_of_component(component_);
                detached_ = actor_->release_component(component_);
            }
        }
    }

    Actor* actor_ = nullptr;
    ActorComponent* component_ = nullptr;
    std::unique_ptr<ActorComponent> detached_;
    size_t detached_index_ = static_cast<size_t>(-1);
    bool present_after_redo_ = true;
    std::string type_label_;
};

// --- Grouping ----------------------------------------------------------------

// Several commands that must undo as one. Children undo in reverse order, which is
// the only order that is correct in general: a later command may depend on what an
// earlier one did.
class CompoundCommand : public Command {
public:
    explicit CompoundCommand(std::string label) : label_(std::move(label)) {}

    void add(std::unique_ptr<Command> cmd) {
        if (cmd) children_.push_back(std::move(cmd));
    }

    bool empty() const { return children_.empty(); }
    size_t size() const { return children_.size(); }
    // When a transaction collected exactly one command there is no reason to keep
    // the wrapper: the child's own name is more specific than the transaction label.
    std::unique_ptr<Command> take_single() {
        if (children_.size() != 1) return nullptr;
        return std::move(children_[0]);
    }

    void undo() override {
        for (size_t i = children_.size(); i-- > 0;) children_[i]->undo();
    }
    void redo() override {
        for (auto& c : children_) c->redo();
    }
    std::string name() const override { return label_; }

private:
    std::string label_;
    std::vector<std::unique_ptr<Command>> children_;
};

// --- The stack ---------------------------------------------------------------

class CommandStack {
public:
    // Older entries are dropped past this depth. The cap matters more here than in
    // most editors: a delete command owns the deleted actor, so an unbounded stack
    // is also an unbounded scene-sized leak.
    static constexpr size_t kDefaultLimit = 128;

    // Opens a transaction. Nested calls are counted, so a helper that brackets its
    // own edit can be called from inside an already-open interaction without
    // splitting it. The name of the outermost open transaction is the one used.
    void begin_transaction(const std::string& label);
    // Closes the innermost transaction. At depth zero the collected commands become
    // a single undo entry; a transaction that collected nothing pushes nothing.
    void end_transaction();
    // Closes and discards without pushing. For an interaction the user cancelled.
    void abort_transaction();
    bool transaction_open() const { return depth_ > 0; }

    // Adds a command. Inside a transaction it joins that transaction instead of
    // becoming its own entry. The command is assumed to be already applied.
    void push(std::unique_ptr<Command> cmd);

    bool undo();
    bool redo();
    bool can_undo() const { return !undo_stack_.empty(); }
    bool can_redo() const { return !redo_stack_.empty(); }

    // Walks the stack so the Undo History panel can jump to any point.
    // `target_depth` is the number of entries that should remain applied.
    void set_depth(size_t target_depth);

    void clear();

    size_t undo_count() const { return undo_stack_.size(); }
    size_t redo_count() const { return redo_stack_.size(); }
    // Entry i of the undo stack, 0 being the oldest.
    std::string undo_name(size_t i) const;
    // Entry i of the redo stack in the order it would be re-applied.
    std::string redo_name(size_t i) const;

    void set_limit(size_t limit) { limit_ = limit ? limit : 1; trim(); }

private:
    void trim();

    std::vector<std::unique_ptr<Command>> undo_stack_;
    std::vector<std::unique_ptr<Command>> redo_stack_;
    std::unique_ptr<CompoundCommand> open_;
    int depth_ = 0;
    size_t limit_ = kDefaultLimit;
};
