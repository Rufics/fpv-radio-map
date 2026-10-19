#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace rfmap {

/// A fixed set of worker threads taking tasks in order.
class ThreadPool {
public:
    explicit ThreadPool(int threads) {
        for (int i = 0; i < std::max(1, threads); ++i) {
            workers_.emplace_back([this](const std::stop_token& stop) { loop(stop); });
        }
    }
    /// Runs the queued tasks to the end, then joins the workers.
    ~ThreadPool() {
        for (auto& t : workers_) t.request_stop();
        for (auto& t : workers_) t.join();
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template <class Task>
    std::future<void> submit(Task task) {
        auto p = std::make_shared<std::packaged_task<void()>>(std::move(task));
        auto f = p->get_future();
        {
            std::lock_guard lock(mutex_);
            tasks_.emplace_back([p] { (*p)(); });
        }
        wake_.notify_one();
        return f;
    }

    [[nodiscard]] int size() const { return static_cast<int>(workers_.size()); }

private:
    void loop(const std::stop_token& stop) {
        while (true) {
            std::function<void()> task;
            // Wakes on a task or on the stop request; a stopping worker still drains the queue.
            if (std::unique_lock lock(mutex_); wake_.wait(lock, stop, [this] { return !tasks_.empty(); })) {
                task = std::move(tasks_.front());
                tasks_.pop_front();
            } else {
                return;
            }
            task();
        }
    }

    std::deque<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable_any wake_;  // NOSONAR(cpp:S8467): only the _any variant can wait on a stop_token
    /// Declared last: destroyed first, so the workers stop before the queue they use goes.
    std::vector<std::jthread> workers_;
};

}  // namespace rfmap
