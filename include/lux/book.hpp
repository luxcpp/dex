#ifndef LUX_BOOK_HPP
#define LUX_BOOK_HPP

#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include <functional>

#include "types.hpp"
#include "orderbook.hpp"
#include "engine.hpp"

// Hash specialization for CLOID (must be before lux namespace)
namespace std {
template<>
struct hash<std::array<uint8_t, 16>> {
    size_t operator()(const std::array<uint8_t, 16>& arr) const noexcept {
        size_t h = 0;
        for (auto b : arr) {
            h = h * 31 + b;
        }
        return h;
    }
};
} // namespace std

namespace lux {

// =============================================================================
// Market Configuration for CLOB
// =============================================================================

struct BookMarketConfig {
    uint32_t market_id;
    uint64_t symbol_id;          // Maps to internal engine symbol
    Currency base_currency;
    Currency quote_currency;
    I128 tick_size_x18;          // Minimum price increment
    I128 lot_size_x18;           // Minimum order size
    I128 min_notional_x18;       // Minimum order notional
    I128 max_order_size_x18;
    bool post_only_mode;         // Only allow maker orders
    bool reduce_only_mode;       // Only allow reducing positions
    uint8_t status;              // 0=inactive, 1=active, 2=cancel-only
};

// =============================================================================
// Order Status Enum (matches Solidity)
// =============================================================================

enum class BookOrderStatus : uint8_t {
    NEW = 0,
    OPEN = 1,
    FILLED = 2,
    CANCELLED = 3,
    REJECTED = 4,
    EXPIRED = 5,
    TRIGGERED = 6
};

// =============================================================================
// Detailed Order State
// =============================================================================

struct BookOrderState {
    uint64_t oid;                // Order ID
    std::array<uint8_t, 16> cloid;  // Client order ID
    uint32_t market_id;
    bool is_buy;
    OrderKind kind;
    TIF tif;
    I128 original_size_x18;
    I128 remaining_size_x18;
    I128 filled_size_x18;
    I128 limit_price_x18;
    I128 trigger_price_x18;
    I128 avg_fill_price_x18;
    BookOrderStatus status;
    uint64_t created_at;
    uint64_t updated_at;
    uint8_t flags;               // fill_flags
};

// =============================================================================
// Execute Action Result
// =============================================================================

struct ExecuteResult {
    int32_t error_code;
    std::vector<uint8_t> result_data;
    std::vector<Trade> trades;
};

// =============================================================================
// LXBook - CLOB Matching Engine Wrapper (LP-9020)
// =============================================================================

class LXBook {
public:
    LXBook();
    ~LXBook() = default;

    // Non-copyable
    LXBook(const LXBook&) = delete;
    LXBook& operator=(const LXBook&) = delete;

    // =========================================================================
    // Market Management
    // =========================================================================

    int32_t create_market(const BookMarketConfig& config);
    int32_t update_market_config(const BookMarketConfig& config);
    std::optional<BookMarketConfig> get_market_config(uint32_t market_id) const;
    uint8_t get_market_status(uint32_t market_id) const;
    bool market_exists(uint32_t market_id) const;

    // =========================================================================
    // Execute Interface (Hyperliquid-style batch execution)
    // =========================================================================

    // Execute single action
    ExecuteResult execute(const LXAccount& sender, const LXAction& action);

    // Execute batch of actions atomically
    std::vector<ExecuteResult> execute_batch(const LXAccount& sender,
                                              const std::vector<LXAction>& actions);

    // =========================================================================
    // Order Operations
    // =========================================================================

    // Place a new order
    LXPlaceResult place_order(const LXAccount& sender, const LXOrder& order);

    // Cancel order by OID
    int32_t cancel_order(const LXAccount& sender, uint32_t market_id, uint64_t oid);

    // Cancel order by client order ID
    int32_t cancel_by_cloid(const LXAccount& sender, uint32_t market_id,
                            const std::array<uint8_t, 16>& cloid);

    // Cancel all orders for a market
    int32_t cancel_all(const LXAccount& sender, uint32_t market_id);

    // Amend order price/size
    LXPlaceResult amend_order(const LXAccount& sender, uint32_t market_id,
                               uint64_t oid, I128 new_size_x18, I128 new_price_x18);

    // =========================================================================
    // Order Queries
    // =========================================================================

    // Get order by OID
    std::optional<BookOrderState> get_order(uint32_t market_id, uint64_t oid) const;

    // Get order by client order ID
    std::optional<BookOrderState> get_order_by_cloid(uint32_t market_id,
                                                      const std::array<uint8_t, 16>& cloid) const;

    // Get all orders for an account
    std::vector<BookOrderState> get_orders(const LXAccount& account, uint32_t market_id) const;

    // Get all open orders for an account across all markets
    std::vector<BookOrderState> get_all_orders(const LXAccount& account) const;

    // =========================================================================
    // Market Data
    // =========================================================================

    // Get L1 (best bid/ask)
    LXL1 get_l1(uint32_t market_id) const;

    // Get depth (multiple levels)
    MarketDepth get_depth(uint32_t market_id, size_t levels = 10) const;

    // Get last trade
    std::optional<Trade> get_last_trade(uint32_t market_id) const;

    // Get recent trades
    std::vector<Trade> get_recent_trades(uint32_t market_id, size_t count = 100) const;

    // =========================================================================
    // HFT Interface (Packed ABI for low latency)
    // =========================================================================

    // Packed execute for colo participants
    // Input: packed binary format for minimal parsing overhead
    // Output: packed binary result
    std::vector<uint8_t> execute_packed(const std::vector<uint8_t>& packed_data);

    // Batch packed execute
    std::vector<uint8_t> execute_batch_packed(const std::vector<uint8_t>& packed_data);

    // =========================================================================
    // Settlement Integration
    // =========================================================================

    // Set settlement callback (called on each fill for LXVault integration)
    using SettlementCallback = std::function<int32_t(const std::vector<Trade>&)>;
    void set_settlement_callback(SettlementCallback callback);

    // =========================================================================
    // Statistics
    // =========================================================================

    struct Stats {
        uint64_t total_markets;
        uint64_t total_orders_placed;
        uint64_t total_orders_cancelled;
        uint64_t total_orders_filled;
        uint64_t total_trades;
        I128 total_volume_x18;
    };
    Stats get_stats() const;

    // =========================================================================
    // Direct Engine Access (for advanced use)
    // =========================================================================

    Engine* get_engine() { return &engine_; }
    const Engine* get_engine() const { return &engine_; }

private:
    // Core matching engine
    Engine engine_;

    // Market configurations
    std::unordered_map<uint32_t, BookMarketConfig> markets_;
    std::unordered_map<uint32_t, uint64_t> market_to_symbol_;  // market_id -> symbol_id
    mutable std::shared_mutex markets_mutex_;

    // Order state tracking
    struct AccountOrders {
        std::unordered_map<uint64_t, BookOrderState> orders;  // oid -> state
        std::unordered_map<std::array<uint8_t, 16>, uint64_t,
            std::hash<std::array<uint8_t, 16>>> cloid_to_oid;
    };
    std::unordered_map<uint64_t, AccountOrders> account_orders_;  // account_hash -> orders
    mutable std::shared_mutex orders_mutex_;

    // Last trade per market
    std::unordered_map<uint32_t, Trade> last_trades_;
    std::unordered_map<uint32_t, std::vector<Trade>> recent_trades_;
    mutable std::shared_mutex trades_mutex_;

    // Settlement callback
    SettlementCallback settlement_callback_;

    // Statistics
    std::atomic<uint64_t> total_orders_placed_{0};
    std::atomic<uint64_t> total_orders_filled_{0};

    // Internal trade listener
    class BookTradeListener : public TradeListener {
    public:
        explicit BookTradeListener(LXBook* book) : book_(book) {}

        void on_trade(const Trade& trade) override;
        void on_order_filled(const Order& order) override;
        void on_order_partially_filled(const Order& order, Quantity fill_qty) override;
        void on_order_cancelled(const Order& order) override;

    private:
        LXBook* book_;
    };
    std::unique_ptr<BookTradeListener> trade_listener_;

    // Internal helpers
    uint64_t get_symbol_id(uint32_t market_id) const;
    Order convert_to_internal(const LXOrder& order, uint64_t symbol_id,
                               const LXAccount& sender) const;
    void update_order_state(const LXAccount& account, uint64_t oid,
                            const std::function<void(BookOrderState&)>& updater);
    void record_trade(uint32_t market_id, const Trade& trade);

    // Action handlers
    ExecuteResult handle_place(const LXAccount& sender, const std::vector<uint8_t>& data);
    ExecuteResult handle_cancel(const LXAccount& sender, const std::vector<uint8_t>& data);
    ExecuteResult handle_cancel_by_cloid(const LXAccount& sender, const std::vector<uint8_t>& data);
    ExecuteResult handle_modify(const LXAccount& sender, const std::vector<uint8_t>& data);
};

// =============================================================================
// Packed Data Format (for HFT)
// =============================================================================
//
// PlaceOrder (32 bytes packed):
//   [0:4]   market_id (uint32)
//   [4:5]   flags (uint8: is_buy, kind, tif, reduce_only)
//   [5:13]  size (int64, scaled by 1e8)
//   [13:21] limit_price (int64, scaled by 1e8)
//   [21:29] trigger_price (int64, scaled by 1e8)
//   [29:32] reserved
//
// CancelOrder (12 bytes packed):
//   [0:4]   market_id (uint32)
//   [4:12]  oid (uint64)
//
// =============================================================================

namespace packed {

struct PackedPlaceOrder {
    uint32_t market_id;
    uint8_t flags;
    int64_t size;
    int64_t limit_price;
    int64_t trigger_price;
} __attribute__((packed));

struct PackedCancelOrder {
    uint32_t market_id;
    uint64_t oid;
} __attribute__((packed));

struct PackedPlaceResult {
    uint64_t oid;
    uint8_t status;
    int64_t filled_size;
    int64_t avg_price;
} __attribute__((packed));

// Flag bit positions
constexpr uint8_t FLAG_IS_BUY = 0x01;
constexpr uint8_t FLAG_KIND_MASK = 0x0E;  // bits 1-3
constexpr uint8_t FLAG_KIND_SHIFT = 1;
constexpr uint8_t FLAG_TIF_MASK = 0x30;   // bits 4-5
constexpr uint8_t FLAG_TIF_SHIFT = 4;
constexpr uint8_t FLAG_REDUCE_ONLY = 0x40;

} // namespace packed

} // namespace lux

#endif // LUX_BOOK_HPP
