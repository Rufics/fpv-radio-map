// ConnectionPool over fake connections: bounded size, lazy growth, lease timeouts, broken and
// retired connections, idle eviction, lifetime and the ping before a lease after a long rest.

#include "rfdb/connection_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

using namespace rfdb;
using namespace std::chrono_literals;

namespace {

/// Shared by a factory and the connections it opened, so a test can break or count them.
struct FakeServer {
    std::atomic<int> opened{0};
    std::atomic<int> alive{0};
    std::atomic<int> pings{0};
    std::atomic<bool> refuse{false};
    std::atomic<bool> dropAll{false};  ///< every session is gone, but the client does not know yet
};

class FakeConnection final : public Connection {
public:
    explicit FakeConnection(FakeServer& server) : server_(server) { ++server_.alive; }
    ~FakeConnection() override { --server_.alive; }
    FakeConnection(const FakeConnection&) = delete;
    FakeConnection& operator=(const FakeConnection&) = delete;
    FakeConnection(FakeConnection&&) = delete;
    FakeConnection& operator=(FakeConnection&&) = delete;

    [[nodiscard]] bool isOpen() const override { return open_; }
    [[nodiscard]] bool ping() override {
        ++server_.pings;
        if (server_.dropAll) open_ = false;
        return open_;
    }
    void close() { open_ = false; }

private:
    FakeServer& server_;
    bool open_ = true;
};

class FakeFactory final : public ConnectionFactory {
public:
    explicit FakeFactory(FakeServer& server) : server_(server) {}
    [[nodiscard]] std::unique_ptr<Connection> open() override {
        if (server_.refuse) throw ConnectionFailed("refused");
        ++server_.opened;
        return std::make_unique<FakeConnection>(server_);
    }

private:
    FakeServer& server_;
};

PoolSettings quiet(PoolSettings s) {
    s.maintenanceInterval = 0ms;  // the tests call maintain() themselves
    return s;
}

class ConnectionPoolTest : public ::testing::Test {
protected:
    std::unique_ptr<ConnectionPool> make(PoolSettings s) {
        return std::make_unique<ConnectionPool>(std::make_unique<FakeFactory>(server_), quiet(s));
    }
    FakeServer server_;
};

}  // namespace

TEST_F(ConnectionPoolTest, OpensOnlyMinIdleAtStartAndGrowsOnDemand) {
    auto pool = make({.maxSize = 4, .minIdle = 1});
    EXPECT_EQ(server_.opened, 1);
    {
        const ConnectionPool::Lease a = pool->acquire();
        const ConnectionPool::Lease b = pool->acquire();
        EXPECT_EQ(server_.opened, 2);
        EXPECT_EQ(pool->stats().open, 2);
        EXPECT_EQ(pool->stats().idle, 0);
    }
    // Both came back and are reused: no further connects.
    const ConnectionPool::Lease c = pool->acquire();
    EXPECT_EQ(server_.opened, 2);
    EXPECT_EQ(pool->stats().idle, 1);
    EXPECT_EQ(pool->stats().leases, 3u);
}

TEST_F(ConnectionPoolTest, NeverOpensMoreThanMaxSizeAndTimesOut) {
    auto pool = make({.maxSize = 2, .minIdle = 0, .leaseTimeout = 30ms});
    const ConnectionPool::Lease a = pool->acquire();
    const ConnectionPool::Lease b = pool->acquire();
    const auto started = Clock::now();
    EXPECT_THROW((void)pool->acquire(), PoolExhausted);
    EXPECT_GE(Clock::now() - started, 30ms);
    EXPECT_EQ(server_.opened, 2);
    EXPECT_EQ(pool->stats().leaseTimeouts, 1u);
    EXPECT_EQ(pool->stats().waiting, 0);
}

TEST_F(ConnectionPoolTest, AWaiterGetsTheConnectionAnotherCallerReturns) {
    auto pool = make({.maxSize = 1, .minIdle = 1, .leaseTimeout = 2s});
    std::optional<std::future<void>> waiter;
    {
        const ConnectionPool::Lease held = pool->acquire();
        waiter = std::async(std::launch::async, [&pool] { const ConnectionPool::Lease got = pool->acquire(); });
        while (pool->stats().waiting == 0) std::this_thread::yield();
    }
    EXPECT_EQ(waiter->wait_for(2s), std::future_status::ready);
    waiter->get();
    EXPECT_EQ(server_.opened, 1);
}

TEST_F(ConnectionPoolTest, ABrokenConnectionIsClosedAndReplacedByTheNextCaller) {
    auto pool = make({.maxSize = 1, .minIdle = 1});
    {
        ConnectionPool::Lease lease = pool->acquire();
        lease.markBroken();
    }
    EXPECT_EQ(pool->stats().open, 0);
    EXPECT_EQ(server_.alive, 0);
    const ConnectionPool::Lease next = pool->acquire();
    EXPECT_EQ(server_.opened, 2);
    const PoolStats s = pool->stats();
    EXPECT_EQ(s.broken, 1u);
    EXPECT_EQ(s.closed, 1u);
}

TEST_F(ConnectionPoolTest, AConnectionThatClosedItselfCountsAsBroken) {
    auto pool = make({.maxSize = 1, .minIdle = 1});
    {
        const ConnectionPool::Lease lease = pool->acquire();
        dynamic_cast<FakeConnection&>(lease.connection()).close();
    }
    EXPECT_EQ(pool->stats().broken, 1u);
    EXPECT_EQ(server_.alive, 0);
}

TEST_F(ConnectionPoolTest, ARetiredConnectionIsClosedWithoutCountingAsBroken) {
    auto pool = make({.maxSize = 1, .minIdle = 1});
    {
        ConnectionPool::Lease lease = pool->acquire();
        lease.retire();
    }
    const PoolStats s = pool->stats();
    EXPECT_EQ(s.closed, 1u);
    EXPECT_EQ(s.broken, 0u);
    EXPECT_EQ(s.open, 0);
}

TEST_F(ConnectionPoolTest, AFailedConnectGivesTheRoomBack) {
    auto pool = make({.maxSize = 1, .minIdle = 0});
    server_.refuse = true;
    EXPECT_THROW((void)pool->acquire(), ConnectionFailed);
    EXPECT_EQ(pool->stats().open, 0);
    EXPECT_EQ(pool->stats().connectFailures, 1u);
    EXPECT_EQ(pool->stats().leases, 0u);
    server_.refuse = false;
    const ConnectionPool::Lease lease = pool->acquire();
    EXPECT_EQ(pool->stats().open, 1);
}

TEST_F(ConnectionPoolTest, AnUnreachableDatabaseFailsTheStart) {
    server_.refuse = true;
    EXPECT_THROW(make({.maxSize = 2, .minIdle = 1}), ConnectionFailed);
}

TEST_F(ConnectionPoolTest, PingsAfterALongRestAndReplacesADroppedSession) {
    auto pool = make({.maxSize = 1, .minIdle = 1, .validateAfterIdle = 0ms});
    server_.dropAll = true;
    const ConnectionPool::Lease lease = pool->acquire();
    EXPECT_EQ(server_.pings, 1);
    EXPECT_EQ(server_.opened, 2);  // the dropped one was replaced, the caller never saw it
    EXPECT_EQ(pool->stats().broken, 1u);
    EXPECT_EQ(pool->stats().leases, 1u);  // one caller, one lease, however many tries
}

TEST_F(ConnectionPoolTest, SkipsThePingForARecentlyUsedConnection) {
    auto pool = make({.maxSize = 1, .minIdle = 1, .validateAfterIdle = 1h});
    { const ConnectionPool::Lease lease = pool->acquire(); }
    { const ConnectionPool::Lease lease = pool->acquire(); }
    EXPECT_EQ(server_.pings, 0);
}

TEST_F(ConnectionPoolTest, MaintainClosesIdleConnectionsDownToMinIdle) {
    auto pool = make({.maxSize = 3, .minIdle = 1, .idleTimeout = 1ms});
    {
        const ConnectionPool::Lease a = pool->acquire();
        const ConnectionPool::Lease b = pool->acquire();
        const ConnectionPool::Lease c = pool->acquire();
    }
    EXPECT_EQ(pool->stats().idle, 3);
    std::this_thread::sleep_for(5ms);
    pool->maintain();
    EXPECT_EQ(pool->stats().open, 1);
    EXPECT_EQ(server_.alive, 1);
}

TEST_F(ConnectionPoolTest, MaintainRecyclesOldConnectionsAndTopsUp) {
    auto pool = make({.maxSize = 2, .minIdle = 2, .maxLifetime = 1ms});
    EXPECT_EQ(server_.opened, 2);
    std::this_thread::sleep_for(5ms);
    pool->maintain();
    EXPECT_EQ(server_.opened, 4);
    EXPECT_EQ(pool->stats().open, 2);
    EXPECT_EQ(server_.alive, 2);
}

TEST_F(ConnectionPoolTest, AnOldConnectionIsNotLentAgain) {
    auto pool = make({.maxSize = 1, .minIdle = 1, .maxLifetime = 1ms});
    std::this_thread::sleep_for(5ms);
    const ConnectionPool::Lease lease = pool->acquire();
    EXPECT_EQ(server_.opened, 2);
    EXPECT_EQ(pool->stats().broken, 0u);
}

TEST_F(ConnectionPoolTest, MaintainKeepsTryingAfterAFailedTopUp) {
    auto topped = make({.maxSize = 2, .minIdle = 2});
    server_.refuse = true;
    {
        ConnectionPool::Lease a = topped->acquire();
        a.markBroken();
    }
    topped->maintain();
    EXPECT_EQ(topped->stats().open, 1);
    EXPECT_EQ(topped->stats().connectFailures, 1u);
    server_.refuse = false;
    topped->maintain();
    EXPECT_EQ(topped->stats().open, 2);
}

TEST_F(ConnectionPoolTest, TheHousekeeperRunsOnItsOwn) {
    ConnectionPool pool(std::make_unique<FakeFactory>(server_),
                        {.maxSize = 2, .minIdle = 1, .idleTimeout = 1ms, .maintenanceInterval = 5ms});
    { const ConnectionPool::Lease a = pool.acquire(), b = pool.acquire(); }
    const auto deadline = Clock::now() + 2s;
    while (pool.stats().open > 1 && Clock::now() < deadline) std::this_thread::sleep_for(2ms);
    EXPECT_EQ(pool.stats().open, 1);
}

TEST_F(ConnectionPoolTest, ManyCallersShareABoundedPool) {
    auto pool = make({.maxSize = 3, .minIdle = 0, .leaseTimeout = 5s});
    std::atomic<int> inside{0};
    std::atomic<int> most{0};
    std::vector<std::jthread> callers;
    callers.reserve(12);
    for (int i = 0; i < 12; ++i) {
        callers.emplace_back([&] {
            for (int n = 0; n < 50; ++n) {
                const ConnectionPool::Lease lease = pool->acquire();
                const int now = ++inside;
                int seen = most.load();
                while (now > seen && !most.compare_exchange_weak(seen, now)) {
                }
                --inside;
            }
        });
    }
    callers.clear();
    EXPECT_LE(most.load(), 3);
    EXPECT_LE(server_.opened, 3);
    EXPECT_EQ(pool->stats().leases, 600u);
}
