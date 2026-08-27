#include "core/asset_database.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// Bumped only if the sidecar layout changes in a way a previous build cannot read.
constexpr int kMetaVersion = 1;

constexpr const char* kMetaSuffix = ".meta";

} // namespace

AssetDatabase& AssetDatabase::get() {
    static AssetDatabase instance;
    return instance;
}

bool AssetDatabase::is_asset_extension(const std::string& extension) {
    // Lowercased, leading dot included. Scene files are deliberately absent: a
    // scene refers to assets, nothing refers to a scene by GUID, so giving them
    // sidecars would add clutter and no resolution.
    static const std::array<const char*, 21> kExtensions = {
        ".mesh", ".terrain", ".gltf", ".glb", ".fbx", ".obj", ".dae",
        ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr",
        ".wav", ".ogg", ".mp3", ".flac",
        ".lua", ".cminus", ".material", ".cpp"
    };
    std::string lowered = extension;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* candidate : kExtensions) {
        if (lowered == candidate) return true;
    }
    return false;
}

std::string AssetDatabase::normalize_path(const std::string& path) {
    if (path.empty()) return {};

    std::error_code ec;
    fs::path p = fs::path(path).lexically_normal();

    // Prefer a project-relative form so a .lithium scene stays portable between
    // machines. Only applied when the asset really is under the working directory;
    // an absolute path elsewhere is left alone rather than rewritten into a chain
    // of "..".
    if (p.is_absolute()) {
        fs::path base = fs::current_path(ec);
        if (!ec) {
            fs::path rel = p.lexically_relative(base);
            // generic_string(), not native(): on Windows native() is a wstring and
            // will not compare against a narrow literal at all.
            const std::string rel_str = rel.generic_string();
            if (!rel_str.empty() && rel_str.rfind("..", 0) != 0) {
                p = rel;
            }
        }
    }

    std::string result = p.generic_string();
    // A leading "./" is noise and would otherwise key the same file twice.
    if (result.rfind("./", 0) == 0) result.erase(0, 2);
    return result;
}

std::string AssetDatabase::generate_guid() {
    // random_device is the OS entropy source. Two 64-bit draws are taken from it
    // directly rather than seeding a PRNG, because 128 bits once per new asset is
    // not a rate that warrants stretching, and a seeded generator would repeat
    // across processes that start in the same instant.
    static std::random_device rd;
    static std::mutex rd_mutex;

    std::uint64_t high = 0;
    std::uint64_t low = 0;
    {
        std::lock_guard<std::mutex> lock(rd_mutex);
        auto draw64 = [&]() {
            std::uint64_t v = rd();
            v = (v << 32) ^ rd();
            return v;
        };
        high = draw64();
        low = draw64();
    }

    static const char* kHex = "0123456789abcdef";
    std::string out(32, '0');
    for (int i = 15; i >= 0; --i) {
        out[i] = kHex[high & 0xF];
        high >>= 4;
    }
    for (int i = 31; i >= 16; --i) {
        out[i] = kHex[low & 0xF];
        low >>= 4;
    }
    return out;
}

std::string AssetDatabase::read_meta(const std::string& normalized_path) {
    const std::string meta_path = normalized_path + kMetaSuffix;
    std::error_code ec;
    if (!fs::exists(meta_path, ec) || ec) return {};

    try {
        std::ifstream in(meta_path);
        if (!in.is_open()) return {};
        json meta;
        in >> meta;

        if (!meta.contains("guid") || !meta["guid"].is_string()) return {};
        std::string guid = meta["guid"].get<std::string>();

        // A GUID is a fixed-width hex token. Anything else came from a corrupt
        // write or a hand edit, and is rejected so the caller mints a fresh one
        // rather than propagating a value the maps cannot round-trip.
        if (guid.size() != 32) return {};
        for (char c : guid) {
            const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            if (!hex) return {};
        }
        return guid;
    } catch (const std::exception& e) {
        std::cerr << "[AssetDatabase] Ignoring malformed " << meta_path << ": "
                  << e.what() << std::endl;
        return {};
    }
}

bool AssetDatabase::write_meta(const std::string& normalized_path, const std::string& guid) {
    const std::string meta_path = normalized_path + kMetaSuffix;
    try {
        json meta;
        meta["version"] = kMetaVersion;
        meta["guid"] = guid;

        std::ofstream out(meta_path, std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[AssetDatabase] Could not write " << meta_path << std::endl;
            return false;
        }
        out << meta.dump(2) << "\n";
        return out.good();
    } catch (const std::exception& e) {
        std::cerr << "[AssetDatabase] Could not write " << meta_path << ": "
                  << e.what() << std::endl;
        return false;
    }
}

std::string AssetDatabase::register_file_locked(const std::string& normalized_path) {
    auto existing = path_to_guid.find(normalized_path);
    if (existing != path_to_guid.end()) return existing->second;

    std::string guid = read_meta(normalized_path);

    // Two assets carrying the same GUID means a file was duplicated along with its
    // sidecar. Whichever is seen first keeps the identity and the other is minted a
    // new one, so a copy-paste in the content folder cannot silently hijack every
    // reference aimed at the original.
    if (!guid.empty()) {
        auto clash = guid_to_path.find(guid);
        if (clash != guid_to_path.end() && clash->second != normalized_path) {
            std::cerr << "[AssetDatabase] " << normalized_path << " duplicates the GUID of "
                      << clash->second << "; assigning it a new one." << std::endl;
            guid.clear();
        }
    }

    if (guid.empty()) {
        guid = generate_guid();
        write_meta(normalized_path, guid);
    }

    guid_to_path[guid] = normalized_path;
    path_to_guid[normalized_path] = guid;
    return guid;
}

void AssetDatabase::scan(const std::vector<std::string>& roots) {
    std::lock_guard<std::mutex> lock(mutex);

    guid_to_path.clear();
    path_to_guid.clear();

    size_t minted = 0;
    for (const std::string& root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec) || ec) continue;

        // The non-throwing overload is deliberate: an unreadable subdirectory
        // should cost that subtree, not the whole scan.
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            std::cerr << "[AssetDatabase] Cannot scan " << root << ": " << ec.message() << std::endl;
            continue;
        }

        for (const auto& entry : it) {
            std::error_code entry_ec;
            if (!entry.is_regular_file(entry_ec) || entry_ec) continue;

            const fs::path& p = entry.path();
            if (p.extension() == kMetaSuffix) continue;
            if (!is_asset_extension(p.extension().string())) continue;

            const std::string normalized = normalize_path(p.string());
            if (normalized.empty()) continue;

            const bool had_meta = !read_meta(normalized).empty();
            register_file_locked(normalized);
            if (!had_meta) ++minted;
        }
    }

    std::cout << "[AssetDatabase] " << path_to_guid.size() << " assets indexed";
    if (minted > 0) std::cout << ", " << minted << " newly identified";
    std::cout << "." << std::endl;
}

std::string AssetDatabase::guid_for_path(const std::string& path) {
    if (path.empty()) return {};

    const std::string normalized = normalize_path(path);
    std::lock_guard<std::mutex> lock(mutex);

    auto it = path_to_guid.find(normalized);
    if (it != path_to_guid.end()) return it->second;

    // Only mint for something that is actually on disk. Handing an identity to a
    // path that does not exist would put an entry in the map that no scan can ever
    // confirm, and would write a .meta next to nothing.
    std::error_code ec;
    if (!fs::exists(normalized, ec) || ec) return {};

    return register_file_locked(normalized);
}

std::string AssetDatabase::path_for_guid(const std::string& guid) const {
    if (guid.empty()) return {};
    std::lock_guard<std::mutex> lock(mutex);
    auto it = guid_to_path.find(guid);
    return it == guid_to_path.end() ? std::string() : it->second;
}

std::string AssetDatabase::resolve(const std::string& guid, const std::string& path_hint) const {
    if (!guid.empty()) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = guid_to_path.find(guid);
        if (it != guid_to_path.end()) return it->second;
    }
    // Either no GUID was stored (a scene from before this existed) or it names an
    // asset that is no longer indexed. The hint is returned untouched so the
    // caller's existing missing-file path reports the name the user recognises.
    return path_hint;
}

std::string AssetDatabase::guid_key_for(const std::string& key) {
    const std::string suffix = "_path";
    if (key.size() > suffix.size() &&
        key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return key.substr(0, key.size() - suffix.size()) + "_guid";
    }
    return key + "_guid";
}

void AssetDatabase::write_ref(json& j, const std::string& key, const std::string& path) {
    j[key] = path;

    const std::string guid = guid_for_path(path);
    if (!guid.empty()) {
        j[guid_key_for(key)] = guid;
    } else {
        // Leave no stale GUID behind when an asset reference is cleared or points
        // at something missing, or the next load would resolve the old target.
        j.erase(guid_key_for(key));
    }
}

std::string AssetDatabase::read_ref(const json& j, const std::string& key,
                                    const std::string& fallback) const {
    std::string path_hint = fallback;
    if (j.contains(key) && j[key].is_string()) {
        path_hint = j[key].get<std::string>();
    }

    std::string guid;
    const std::string guid_key = guid_key_for(key);
    if (j.contains(guid_key) && j[guid_key].is_string()) {
        guid = j[guid_key].get<std::string>();
    }

    return resolve(guid, path_hint);
}

void AssetDatabase::notify_moved(const std::string& old_path, const std::string& new_path) {
    const std::string from = normalize_path(old_path);
    const std::string to = normalize_path(new_path);
    if (from.empty() || to.empty() || from == to) return;

    std::lock_guard<std::mutex> lock(mutex);
    auto it = path_to_guid.find(from);
    if (it == path_to_guid.end()) return;

    const std::string guid = it->second;
    path_to_guid.erase(it);
    path_to_guid[to] = guid;
    guid_to_path[guid] = to;
}

void AssetDatabase::forget(const std::string& path) {
    const std::string normalized = normalize_path(path);
    if (normalized.empty()) return;

    std::lock_guard<std::mutex> lock(mutex);
    auto it = path_to_guid.find(normalized);
    if (it == path_to_guid.end()) return;

    guid_to_path.erase(it->second);
    path_to_guid.erase(it);
}

size_t AssetDatabase::asset_count() const {
    std::lock_guard<std::mutex> lock(mutex);
    return path_to_guid.size();
}
