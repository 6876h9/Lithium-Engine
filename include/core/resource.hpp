#pragma once

#include <string>
#include <atomic>

enum class ResourceState {
    Unloaded,
    Loading,
    LoadedCPU,
    LoadedGPU,
    Failed
};

class Resource {
public:
    Resource(const std::string& filepath) : filepath(filepath) {}
    virtual ~Resource() = default;

    // Called on a background worker thread
    virtual bool load_from_disk() = 0;

    // Called on the main thread
    virtual bool upload_to_gpu() = 0;

    const std::string& get_filepath() const { return filepath; }
    ResourceState get_state() const { return state.load(); }
    void set_state(ResourceState new_state) { state.store(new_state); }

protected:
    std::string filepath;
    std::atomic<ResourceState> state{ResourceState::Unloaded};
};
