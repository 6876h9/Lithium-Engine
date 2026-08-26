#include "core/resource_manager.hpp"
#include <chrono>
#include <filesystem>
#include "core/model_importer.hpp"

void ResourceManager::initialize(size_t thread_count) {
    for (size_t i = 0; i < thread_count; ++i) {
        workers.emplace_back([this] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex);
                    this->condition.wait(lock, [this] { return this->stop || !this->task_queue.empty(); });
                    if (this->stop && this->task_queue.empty()) {
                        return;
                    }
                    task = std::move(this->task_queue.front());
                    this->task_queue.pop();
                }
                task();
            }
        });
    }
}

void ResourceManager::shutdown() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers.clear();
}

void ResourceManager::enqueue_task(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (stop) {
            throw std::runtime_error("enqueue on stopped ResourceManager");
        }
        task_queue.push(std::move(task));
    }
    condition.notify_one();
}

void ResourceManager::watch_model_source(const std::string& source_path, const std::string& mesh_path) {
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(source_path, ec);
    if (ec) return;
    std::lock_guard<std::mutex> lock(watch_mutex);
    model_sources[source_path] = mesh_path;
    watch_times[source_path] = mtime;
}

// Re-runs the normal load path for an already-cached resource. Deliberately reuses
// the same worker-thread load plus main-thread upload the initial load uses, so a
// reload cannot diverge from a cold load.
void ResourceManager::queue_reload(const std::shared_ptr<Resource>& resource) {
    if (!resource) return;
    resource->set_state(ResourceState::Loading);
    enqueue_task([this, resource]() {
        if (resource->load_from_disk()) {
            resource->set_state(ResourceState::LoadedCPU);
            std::lock_guard<std::mutex> u_lock(upload_mutex);
            upload_queue.push(resource);
        } else {
            resource->set_state(ResourceState::Failed);
        }
    });
}

// Polls modification times of everything loaded.
//
// Polling rather than inotify/ReadDirectoryChangesW: it is identical on Linux and
// Windows, needs no platform code, and the asset count here is small enough that
// stat-ing them a couple of times a second is free. A native watcher would only be
// worth it with thousands of files.
void ResourceManager::check_hot_reload() {
    if (!hot_reload_enabled) return;

    auto now = std::chrono::steady_clock::now();
    if (now - last_watch_check < std::chrono::milliseconds(500)) return;
    last_watch_check = now;

    // Snapshot what to check, so the maps are not held while reloading.
    std::vector<std::pair<std::string, std::string>> changed_sources;
    std::vector<std::string> changed_assets;
    {
        std::lock_guard<std::mutex> lock(watch_mutex);
        for (auto& [path, last_time] : watch_times) {
            std::error_code ec;
            auto mtime = std::filesystem::last_write_time(path, ec);
            if (ec) continue;                  // deleted or mid-write; try again later
            if (mtime == last_time) continue;
            last_time = mtime;

            auto src = model_sources.find(path);
            if (src != model_sources.end()) {
                changed_sources.emplace_back(path, src->second);
            } else {
                changed_assets.push_back(path);
            }
        }
    }

    // Source models: re-import first, which rewrites the .mesh and in turn trips the
    // watcher for that file on a later pass.
    for (const auto& [source, mesh_path] : changed_sources) {
        std::cout << "[HotReload] Source changed, re-importing: " << source << std::endl;
        if (ModelImporter::import_model(source).empty()) {
            std::cerr << "[HotReload] Re-import failed for " << source << std::endl;
        }
    }

    if (changed_assets.empty()) return;

    for (const std::string& path : changed_assets) {
        std::shared_ptr<Resource> resource;
        {
            std::lock_guard<std::mutex> lock(resources_mutex);
            auto it = resources.find(path);
            if (it == resources.end()) continue;
            resource = it->second;
        }
        // Skip anything already mid-load, or the reload races the in-flight one.
        if (resource->get_state() == ResourceState::Loading) continue;
        std::cout << "[HotReload] Reloading " << path << std::endl;
        queue_reload(resource);
    }
}

void ResourceManager::update() {
    // 0. Reload anything that changed on disk since the last check.
    check_hot_reload();

    // 1. Process Upload Queue
    std::shared_ptr<Resource> res = nullptr;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(upload_mutex);
            if (upload_queue.empty()) break;
            res = upload_queue.front();
            upload_queue.pop();
        }
        
        if (res->get_state() == ResourceState::LoadedCPU) {
            if (res->upload_to_gpu()) {
                res->set_state(ResourceState::LoadedGPU);
            } else {
                res->set_state(ResourceState::Failed);
            }
        }
    }

    // 2. Garbage Collection
    std::vector<std::string> to_remove;
    {
        std::lock_guard<std::mutex> lock(resources_mutex);
        for (auto it = resources.begin(); it != resources.end(); ++it) {
            // use_count() == 1 means ONLY the ResourceManager holds it.
            if (it->second.use_count() == 1 && 
                (it->second->get_state() == ResourceState::LoadedGPU || 
                 it->second->get_state() == ResourceState::Failed)) {
                to_remove.push_back(it->first);
            }
        }

        for (const auto& key : to_remove) {
            std::cout << "[ResourceManager] Garbage Collecting: " << key << std::endl;
            resources.erase(key);
        }
    }
}

void ResourceManager::invalidate_gpu_resources() {
    std::lock_guard<std::mutex> lock(resources_mutex);
    std::lock_guard<std::mutex> u_lock(upload_mutex);

    // Clear upload queue to prevent old context uploads
    std::queue<std::shared_ptr<Resource>> empty_queue;
    std::swap(upload_queue, empty_queue);

    // Requeue all loaded resources
    for (auto& pair : resources) {
        if (pair.second->get_state() == ResourceState::LoadedGPU) {
            pair.second->set_state(ResourceState::LoadedCPU);
            upload_queue.push(pair.second);
        }
    }
}
