#include "rfdb/pg/session_pool.hpp"

#include <memory>
#include <utility>

namespace rfdb::pg {

SessionPool::SessionPool(const std::string& url, SessionOptions options, const PoolSettings& settings)
    : pool_(std::make_unique<SessionFactory>(url, std::move(options)), settings) {}

}  // namespace rfdb::pg
