#pragma once

#include <nlohmann/json.hpp>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Stable identity for assets, so a reference survives the file being renamed or
// moved.
//
// Everything in a scene used to be a raw path string: mesh_path, material_path,
// texture_path, the HDRI, script paths. That works exactly until someone
// reorganises a folder, at which point every scene pointing into it breaks with no
// diagnostic - the path simply fails to load and the object comes back empty.
//
// Each asset gets a sidecar "<file>.meta" holding a generated 128-bit GUID. The
// GUID is what a scene stores; the path is stored alongside it purely so the file
// stays readable and so scenes written before this existed keep working. On load
// the GUID is tried first and the path is the fallback, which means:
//
//   - renaming or moving an asset keeps every reference to it intact
//   - a scene saved by an older build still loads, because the path is still there
//   - an asset with no .meta yet still loads, for the same reason
//
// The database is populated by scanning the content roots at startup. Scanning
// creates any .meta that is missing, so adopting this costs nothing: drop the
// engine on an existing project and the first run mints identities for what is
// already there.
class AssetDatabase {
public:
    static AssetDatabase& get();

    // Walks `roots` recursively, reading each asset's .meta and creating one where
    // it is absent. Safe to call more than once; a second scan refreshes the maps
    // rather than duplicating them.
    void scan(const std::vector<std::string>& roots);

    // GUID for `path`, minting and writing a .meta if the asset does not have one.
    // Returns "" when the file does not exist - callers store the path alone in
    // that case rather than inventing an identity for something absent.
    std::string guid_for_path(const std::string& path);

    // Path currently registered for `guid`, or "" if no scanned asset carries it.
    std::string path_for_guid(const std::string& guid) const;

    // The resolution rule, in one place: prefer the GUID, fall back to the path.
    //
    // A GUID that resolves wins even when it disagrees with path_hint, because
    // disagreement is exactly the case this exists for - the asset moved and the
    // hint is the stale half. When the GUID is unknown (an older scene, an asset
    // with no .meta, or one genuinely deleted) the hint is all there is, so it is
    // returned as-is and the caller's normal missing-file handling takes over.
    std::string resolve(const std::string& guid, const std::string& path_hint) const;

    // --- Serialization helpers ----------------------------------------------
    // Writes both halves of a reference: `key` keeps the path, and a sibling key
    // carries the GUID. The sibling is `key` with a trailing "_path" replaced by
    // "_guid" ("mesh_path" -> "mesh_guid"), or `key` + "_guid" otherwise.
    //
    // Writing the path unconditionally is what keeps a scene saved by this build
    // loadable by an older one.
    void write_ref(nlohmann::json& j, const std::string& key, const std::string& path);

    // Inverse of write_ref. Reads the GUID sibling if present, the plain key
    // otherwise, and runs both through resolve().
    std::string read_ref(const nlohmann::json& j, const std::string& key,
                         const std::string& fallback = "") const;

    // Records that an asset moved, so references resolve to the new location
    // without a full rescan. The .meta travels with the file, so this only updates
    // the in-memory maps.
    void notify_moved(const std::string& old_path, const std::string& new_path);

    // Drops `path` from the maps. The .meta on disk is not touched; a later scan
    // re-adopts the asset with its original GUID if the file comes back.
    void forget(const std::string& path);

    size_t asset_count() const;

    // Name of the sibling key write_ref/read_ref use for `key`. Exposed so callers
    // doing their own JSON can stay consistent with it.
    static std::string guid_key_for(const std::string& key);

    // True for extensions the engine actually loads. Anything else is skipped by
    // scan() so the content tree does not fill up with .meta for README files.
    static bool is_asset_extension(const std::string& extension);

    // The canonical form every path is keyed on: lexically normalized,
    // forward-slashed, and made relative to the working directory when it sits
    // beneath it. Public because anything comparing a path against what resolve()
    // returned has to compare in the same form - "./Content/a.mesh" and
    // "Content/a.mesh" are one asset, and only this decides that.
    static std::string normalize_path(const std::string& path);

private:
    AssetDatabase() = default;
    AssetDatabase(const AssetDatabase&) = delete;
    AssetDatabase& operator=(const AssetDatabase&) = delete;

    // Registers one file, reading or creating its .meta. Assumes the caller holds
    // the mutex.
    std::string register_file_locked(const std::string& normalized_path);

    // Reads the GUID out of "<path>.meta", or "" if the sidecar is missing or
    // malformed. A malformed sidecar is reported and then replaced rather than
    // aborting the scan, because one bad file should not cost the whole project
    // its identities.
    static std::string read_meta(const std::string& normalized_path);
    static bool write_meta(const std::string& normalized_path, const std::string& guid);

    // 32 lowercase hex characters from a 128-bit draw. Not a formatted UUID: this
    // never leaves the project, and an opaque token avoids implying a version or
    // variant the generator does not actually honour.
    static std::string generate_guid();


    mutable std::mutex mutex;
    std::unordered_map<std::string, std::string> guid_to_path;
    std::unordered_map<std::string, std::string> path_to_guid;
};
