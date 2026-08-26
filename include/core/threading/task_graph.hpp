#pragma once

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <queue>
#include <atomic>
#include <future>
#include <memory>

namespace Threading {

class TaskGraph {
public:
    TaskGraph(size_t num_threads = std::thread::hardware_concurrency());
    ~TaskGraph();

    // Prevent copying
    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;

    // Dispatch a task and return a future to wait on its completion
    template<class F, class... Args>
    auto dispatch(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);

            // Don't allow enqueueing after stopping the pool
            if (stop) {
                throw std::runtime_error("dispatch on stopped TaskGraph");
            }

            tasks.emplace([task]() { (*task)(); });
        }
        condition.notify_one();
        return res;
    }

    // Wait until the queue is completely empty and all threads are idle
    void wait_idle();

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex queue_mutex;
    std::condition_variable condition;
    std::condition_variable wait_condition;
    
    std::atomic<bool> stop;
    std::atomic<int> active_tasks;
};

} // namespace Threading
