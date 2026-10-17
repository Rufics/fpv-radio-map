#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace rfmap {

/// A fixed set of worker threads taking tasks in order.
class ThreadPool {
public:
    explicit ThreadPool(int threads) {
        for (int i = 0; i < std::max(1, threads); ++i) {
            workers_.emplace_back([this] { loop(); });
        }
    }
    ~ThreadPool() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& t : workers_) t.join();
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    std::future<void> submit(std::function<void()> task) {
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
    void loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
};

}  // namespace rfmap
