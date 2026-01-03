#ifndef LUX_ENGINE_HPP
#define LUX_ENGINE_HPP

#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include <queue>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <future>

#include "orderbook.hpp"
#include "order.hpp"
#include "trade.hpp"

namespace lux {

// Order submission result
struct OrderResult {
    bool success;
    uint64_t order_id;
    std::string error;
    std::vector<Trade> trades;
};

// Cancel result
struct CancelResult {
    bool success;
    std::optional<Order> cancelled_order;
    std::string error;
};

// Batch order for bulk processing
struct BatchOrder {
    enum class Action { Place, Cancel, Modify };
    Action action;
    Order order;  // For Place
    uint64_t order_id;  // For Cancel/Modify
    Price new_price;  // For Modify
    Quantity new_quantity;  // For Modify
};

struct BatchResult {
    std::vector<OrderResult> order_results;
    std::vector<CancelResult> cancel_results;
    std::vector<Trade> all_trades;
};

// Engine configuration
struct EngineConfig {
    size_t worker_threads = 1;
    size_t max_batch_size = 1000;
    bool enable_self_trade_prevention = true;
    bool async_mode = false;
};

// Trading engine managing multiple orderbooks
class Engine {
public:
    explicit Engine(EngineConfig config = {});
    ~Engine();

    // Non-copyable
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Lifecycle
    void start();
    void stop();
    bool is_running() const { return running_.load(); }

    // Symbol management
    bool add_symbol(uint64_t symbol_id);
    bool remove_symbol(uint64_t symbol_id);
    bool has_symbol(uint64_t symbol_id) const;
    std::vector<uint64_t> symbols() const;

    // Order operations
    OrderResult place_order(Order order);
    CancelResult cancel_order(uint64_t symbol_id, uint64_t order_id);
    OrderResult modify_order(uint64_t symbol_id, uint64_t order_id,
                            Price new_price, Quantity new_quantity);

    // Batch operations
    BatchResult process_batch(const std::vector<BatchOrder>& batch);

    // Query operations
    std::optional<Order> get_order(uint64_t symbol_id, uint64_t order_id) const;
    MarketDepth get_depth(uint64_t symbol_id, size_t levels = 10) const;
    std::optional<Price> best_bid(uint64_t symbol_id) const;
    std::optional<Price> best_ask(uint64_t symbol_id) const;

    // Statistics
    struct Stats {
        uint64_t total_orders_placed;
        uint64_t total_orders_cancelled;
        uint64_t total_trades;
        uint64_t total_volume;
    };
    Stats get_stats() const;

    // Trade listener registration
    void set_trade_listener(TradeListener* listener);

    // Direct orderbook access (use with caution)
    OrderBook* get_orderbook(uint64_t symbol_id);
    const OrderBook* get_orderbook(uint64_t symbol_id) const;

private:
    EngineConfig config_;
    std::atomic<bool> running_{false};

    // Orderbooks by symbol
    std::unordered_map<uint64_t, std::unique_ptr<OrderBook>> orderbooks_;
    mutable std::shared_mutex orderbooks_mutex_;

    // Statistics
    std::atomic<uint64_t> total_orders_placed_{0};
    std::atomic<uint64_t> total_orders_cancelled_{0};
    std::atomic<uint64_t> total_trades_{0};
    std::atomic<uint64_t> total_volume_{0};

    // Trade listener
    TradeListener* trade_listener_{nullptr};

    // Async processing (if enabled)
    struct AsyncOrder {
        BatchOrder batch_order;
        std::promise<OrderResult> promise;
    };
    std::queue<AsyncOrder> order_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::vector<std::thread> worker_threads_;

    void worker_loop();
    OrderResult process_single_order(const BatchOrder& batch_order);
};

// Order ID generator
class OrderIdGenerator {
public:
    static OrderIdGenerator& instance() {
        static OrderIdGenerator gen;
        return gen;
    }

    uint64_t next() {
        return counter_.fetch_add(1, std::memory_order_relaxed);
    }

    void reset(uint64_t start = 1) {
        counter_.store(start, std::memory_order_relaxed);
    }

private:
    OrderIdGenerator() = default;
    std::atomic<uint64_t> counter_{1};
};

} // namespace lux

#endif // LUX_ENGINE_HPP
