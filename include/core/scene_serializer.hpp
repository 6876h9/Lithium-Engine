#pragma once

#include "world/actor.hpp"
#include <vector>
#include <memory>
#include <string>

class SceneSerializer {
public:
    // extra_folders carries Outliner folders that currently hold no actors. Without
    // it a folder someone created and had not filled yet would vanish on save,
    // because nothing would reference it.
    static void save_scene(const std::string& filepath, const std::vector<std::shared_ptr<Actor>>& actors,
                           const std::vector<std::string>& extra_folders = {});
    static bool load_scene(const std::string& filepath, std::vector<std::shared_ptr<Actor>>& out_actors);
    // Folder list from the last load_scene(), including empty ones.
    static const std::vector<std::string>& last_loaded_folders();

    // Prefabs: one actor saved as a reusable asset, instantiated as many times as
    // wanted. Serialised through the same per-actor path as scenes, so a prefab
    // carries mesh, material, lights and physics exactly as a scene entry would.
    static bool save_prefab(const std::string& filepath, Actor* actor);
    static std::shared_ptr<Actor> load_prefab(const std::string& filepath);

    // A deep copy of `actor`, produced by serialising it and reading it straight
    // back. Going through the record rather than copy-constructing is what makes
    // this correct: an Actor owns components with back-pointers to their owner, and
    // a memberwise copy would leave every one of them pointing at the original.
    // Anything a scene file round-trips, a duplicate carries.
    static std::shared_ptr<Actor> clone_actor(Actor* actor);

    // --- Prefab linkage ------------------------------------------------------
    // Writes this actor's current state back over its prefab, so every other
    // instance picks the change up. Returns false if the file could not be written.
    static bool apply_actor_to_prefab(const std::string& filepath, Actor* actor);

    // Names of the properties this actor has changed since it was instantiated,
    // for the editor to show. Empty for an unlinked actor or one still matching its
    // prefab exactly.
    static std::vector<std::string> list_prefab_overrides(Actor* actor);
};
