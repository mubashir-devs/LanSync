#pragma once
// Fixed-size thread pool for blocking work (scanning, hashing, large copies) so
// the networking thread and the UI thread never block on disk.

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace lansync {

class WorkerPool {
public:
    explicit WorkerPool(std::size_t threads);
    ~WorkerPool();   // finishes the task in flight, drops queued tasks, joins

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void post(std::function<void()> task);

private:
    void run(const std::stop_token& stop);

    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::jthread> threads_;
};

}  // namespace lansync
