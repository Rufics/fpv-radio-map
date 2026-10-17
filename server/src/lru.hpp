#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace rfmap {

struct CacheStats {
    std::size_t limitBytes = 0;
    std::size_t usedBytes = 0;
    std::size_t entries = 0;
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;
    [[nodiscard]] double hitRate() const {
        const std::size_t total = hits + misses;
        return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
    }
};

/// Least-recently-used map bounded by the bytes its values report. Thread-safe. Values are
/// shared, so a reader keeps its value alive even if it is evicted meanwhile.
template <class Key, class Value, class Hash = std::hash<Key>>
class LruCache {
public:
    using Sizer = std::function<std::size_t(const Value&)>;

    LruCache(std::size_t limitBytes, Sizer sizer) : limit_(limitBytes), sizer_(std::move(sizer)) {}

    std::shared_ptr<const Value> get(const Key& key) {
        std::lock_guard lock(mutex_);
        const auto it = index_.find(key);
        if (it == index_.end()) {
            ++misses_;
            return nullptr;
        }
        ++hits_;
        order_.splice(order_.begin(), order_, it->second);
        return it->second->value;
    }

    /// Stores a value; one larger than the whole limit is not kept.
    void put(const Key& key, std::shared_ptr<const Value> value) {
        const std::size_t size = sizer_(*value);
        std::lock_guard lock(mutex_);
        if (const auto it = index_.find(key); it != index_.end()) {
            used_ -= it->second->size;
            order_.erase(it->second);
            index_.erase(it);
        }
        if (size > limit_) return;
        order_.push_front({key, std::move(value), size});
        index_[key] = order_.begin();
        used_ += size;
        trim();
    }

    void setLimit(std::size_t bytes) {
        std::lock_guard lock(mutex_);
        limit_ = bytes;
        trim();
    }

    void clear() {
        std::lock_guard lock(mutex_);
        order_.clear();
        index_.clear();
        used_ = 0;
    }

    [[nodiscard]] CacheStats stats() const {
        std::lock_guard lock(mutex_);
        return {limit_, used_, index_.size(), hits_, misses_, evictions_};
    }

private:
    struct Node {
        Key key;
        std::shared_ptr<const Value> value;
        std::size_t size;
    };

    void trim() {
        while (used_ > limit_ && !order_.empty()) {
            used_ -= order_.back().size;
            index_.erase(order_.back().key);
            order_.pop_back();
            ++evictions_;
        }
    }

    std::size_t limit_;
    Sizer sizer_;
    mutable std::mutex mutex_;
    std::list<Node> order_;
    std::unordered_map<Key, typename std::list<Node>::iterator, Hash> index_;
    std::size_t used_ = 0;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
    std::size_t evictions_ = 0;
};

}  // namespace rfmap
