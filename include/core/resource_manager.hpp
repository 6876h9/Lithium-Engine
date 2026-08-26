#pragma once

#include "core/resource.hpp"
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <type_traits>
#include <iostream>
#include <filesystem>
#include <chrono>

class ResourceManager {
public:
    static ResourceManager& get() {
        static ResourceManager instance;
        return instance;
    }

    void initialize(size_t thread_count = 4);
    void shutdown();
    void update(); // Called on main thread to upload to GPU and GC

    // --- Hot reloading -----------------------------------------------------
    // Watches every loaded asset's file and reloads it in place when it changes on
    // disk, so editing a texture or model in another tool shows up in the running
    // viewport without restarting the engine.
    void set_hot_reload_enabled(bool enabled) { hot_reload_enabled = enabled; }
    bool is_hot_reload_enabled() const { return hot_reload_enabled; }

    // Registers a source asset (.gltf/.fbx/...) that was converted into `mesh_path`.
    // Editing the source in Blender re-runs the import and reloads the mesh, which is
    // the workflow that actually matters - the engine itself only ever loads .mesh.
    void watch_model_source(const std::string& source_path, const std::string& mesh_path);
    void invalidate_gpu_resources(); // Called when swapping API

    template<typename T>
    std::shared_ptr<T> load_async(const std::string& filepath) {
        static_assert(std::is_base_of<Resource, T>::value, "T must derive from Resource");

        std::lock_guard<std::mutex> lock(resources_mutex);

        // Return cached resource if it exists
        auto it = resources.find(filepath);
        if (it != resources.end()) {
            return std::static_pointer_cast<T>(it->second);
        }

        // Create new resource
        auto resource = std::make_shared<T>(filepath);
        resource->set_state(ResourceState::Loading);
        resources[filepath] = resource;

        // Baseline for change detection.
        {
            std::error_code ec;
            auto mtime = std::filesystem::last_write_time(filepath, ec);
            if (!ec) {
                std::lock_guard<std::mutex> w_lock(watch_mutex);
                watch_times[filepath] = mtime;
            }
        }

        // Push task to thread pool
        enqueue_task([this, resource]() {
            if (resource->load_from_disk()) {
                resource->set_state(ResourceState::LoadedCPU);
                
                std::lock_guard<std::mutex> u_lock(upload_mutex);
                upload_queue.push(resource);
            } else {
                resource->set_state(ResourceState::Failed);
            }
        });

        return resource;
    }

private:
    ResourceManager() = default;
    ~ResourceManager() { shutdown(); }
    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    void enqueue_task(std::function<void()> task);
    void check_hot_reload();
    void queue_reload(const std::shared_ptr<Resource>& resource);

    bool hot_reload_enabled = true;
    std::chrono::steady_clock::time_point last_watch_check{};
    // Last-seen modification time per watched path.
    std::unordered_map<std::string, std::filesystem::file_time_type> watch_times;
    // Source asset -> generated .mesh produced from it.
    std::unordered_map<std::string, std::string> model_sources;
    std::mutex watch_mutex;

    // Thread pool
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> task_queue;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop = false;

    // Resources map
    std::unordered_map<std::string, std::shared_ptr<Resource>> resources;
    std::mutex resources_mutex;

    // Upload queue for main thread
    std::queue<std::shared_ptr<Resource>> upload_queue;
    std::mutex upload_mutex;
};
