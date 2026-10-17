#pragma once

#include "common.hpp"
#include "database.hpp"
#include "lru.hpp"

#include <optional>
#include <string>

namespace rfmap {

/// Stable SHA-256 over a JSON document's canonical form (nlohmann dumps object keys sorted).
std::string inputHash(const json& inputs);

/// Persistent tier of the result cache, bounded by rows and bytes.
class IResultTable {
public:
    virtual ~IResultTable() = default;
    [[nodiscard]] virtual std::optional<json> get(const std::string& hash) = 0;
    virtual void put(const std::string& hash, const std::string& kind, const json& result, std::size_t bytes) = 0;
    /// Drops the least recently used rows until both limits hold.
    virtual void trim(std::size_t maxRows, std::size_t maxBytes) = 0;
    virtual void clear() = 0;
    struct Usage {
        std::size_t rows;
        std::size_t bytes;
    };
    [[nodiscard]] virtual Usage usage() = 0;
};

class PgResultTable final : public IResultTable {
public:
    explicit PgResultTable(Database& db) : db_(db) {}
    std::optional<json> get(const std::string& hash) override;
    void put(const std::string& hash, const std::string& kind, const json& result, std::size_t bytes) override;
    void trim(std::size_t maxRows, std::size_t maxBytes) override;
    void clear() override;
    Usage usage() override;

private:
    Database& db_;
};

/// Results keyed by the hash of every input that produced them: request, library item
/// versions, terrain file identity, forecast values, module versions and server version.
/// Memory first, then the table; a table hit is promoted to memory.
class ResultCache {
public:
    struct Limits {
        std::size_t memoryBytes;
        std::size_t tableRows;
        std::size_t tableBytes;
    };
    ResultCache(Limits limits, IResultTable* table);

    std::optional<json> get(const std::string& hash);
    void put(const std::string& hash, const std::string& kind, const json& result);

    void setLimits(Limits limits);
    [[nodiscard]] Limits limits() const;
    void clearMemory();
    void clearTable();

    [[nodiscard]] CacheStats memoryStats() const { return memory_.stats(); }
    struct TableStats {
        std::size_t rows, bytes, hits, misses;
    };
    [[nodiscard]] TableStats tableStats() const;

private:
    LruCache<std::string, std::string> memory_;  ///< serialised JSON, so the size is exact
    IResultTable* table_;
    mutable std::mutex mutex_;
    Limits limits_;
    std::size_t tableHits_ = 0;
    std::size_t tableMisses_ = 0;
    std::size_t putsSinceTrim_ = 0;
};

}  // namespace rfmap
