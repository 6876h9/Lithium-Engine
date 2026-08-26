#pragma once

#include "world/actor.hpp"
#include <vector>
#include <memory>
#include <string>

class SceneSerializer {
public:
    static void save_scene(const std::string& filepath, const std::vector<std::shared_ptr<Actor>>& actors);
    static bool load_scene(const std::string& filepath, std::vector<std::shared_ptr<Actor>>& out_actors);

    // Prefabs: one actor saved as a reusable asset, instantiated as many times as
    // wanted. Serialised through the same per-actor path as scenes, so a prefab
    // carries mesh, material, lights and physics exactly as a scene entry would.
    static bool save_prefab(const std::string& filepath, Actor* actor);
    static std::shared_ptr<Actor> load_prefab(const std::string& filepath);

    // --- Prefab linkage ------------------------------------------------------
    // Writes this actor's current state back over its prefab, so every other
    // instance picks the change up. Returns false if the file could not be written.
    static bool apply_actor_to_prefab(const std::string& filepath, Actor* actor);

    // Names of the properties this actor has changed since it was instantiated,
    // for the editor to show. Empty for an unlinked actor or one still matching its
    // prefab exactly.
    static std::vector<std::string> list_prefab_overrides(Actor* actor);
};
