#include "core/common/worker_pool.hpp"

namespace lansync {

WorkerPool::WorkerPool(std::size_t n) {
    for (std::size_t i = 0; i < (n == 0 ? 1 : n); ++i) {
        threads_.emplace_back([this](std::stop_token st) { run(st); });
    }
}

WorkerPool::~WorkerPool() {
    for (auto& t : threads_) t.request_stop();
    cv_.notify_all();
    threads_.clear();   // jthread joins
}

void WorkerPool::post(std::function<void()> task) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void WorkerPool::run(const std::stop_token& stop) {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            if (!cv_.wait(lock, stop, [this] { return !queue_.empty(); })) return;   // stop requested
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}

}  // namespace lansync
