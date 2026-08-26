#include "core/threading/task_graph.hpp"
#include <iostream>

namespace Threading {

TaskGraph::TaskGraph(size_t num_threads) : stop(false), active_tasks(0) {
    // If num_threads is 0 (e.g. hardware_concurrency returned 0), fallback to a reasonable default
    if (num_threads == 0) {
        num_threads = 4;
    }

    for (size_t i = 0; i < num_threads; ++i) {
        workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;

                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex);
                    this->condition.wait(lock, [this] { 
                        return this->stop || !this->tasks.empty(); 
                    });
                    
                    if (this->stop && this->tasks.empty()) {
                        return;
                    }

                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                    this->active_tasks++;
                }

                // Execute task
                task();

                // Notify wait_idle if this was the last active task
                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex);
                    this->active_tasks--;
                    if (this->tasks.empty() && this->active_tasks == 0) {
                        this->wait_condition.notify_all();
                    }
                }
            }
        });
    }
}

TaskGraph::~TaskGraph() {
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
}

void TaskGraph::wait_idle() {
    std::unique_lock<std::mutex> lock(queue_mutex);
    wait_condition.wait(lock, [this] {
        return this->tasks.empty() && this->active_tasks == 0;
    });
}

} // namespace Threading
