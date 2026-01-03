#ifndef LUX_ORDERBOOK_HPP
#define LUX_ORDERBOOK_HPP

#include <map>
#include <list>
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include <vector>
#include <atomic>
#include <functional>

#include "order.hpp"
#include "trade.hpp"

namespace lux {

// Price level containing orders at a single price point
// Orders are in FIFO queue for price-time priority
struct PriceLevel {
    Price price;
    std::list<Order> orders;
    Quantity total_quantity{0};

    size_t order_count() const { return orders.size(); }

    void add_order(Order order) {
        total_quantity += order.remaining();
        orders.push_back(std::move(order));
    }

    // Remove order by ID, returns true if found
    bool remove_order(uint64_t order_id) {
        for (auto it = orders.begin(); it != orders.end(); ++it) {
            if (it->id == order_id) {
                total_quantity -= it->remaining();
                orders.erase(it);
                return true;
            }
        }
        return false;
    }

    // Get front order (best time priority at this price)
    Order* front() {
        return orders.empty() ? nullptr : &orders.front();
    }

    const Order* front() const {
        return orders.empty() ? nullptr : &orders.front();
    }

    void pop_front() {
        if (!orders.empty()) {
            total_quantity -= orders.front().remaining();
            orders.pop_front();
        }
    }

    bool empty() const { return orders.empty(); }
};

// Market depth snapshot for a single side
struct DepthLevel {
    double price;
    double quantity;
    int order_count;
};

struct MarketDepth {
    std::vector<DepthLevel> bids;
    std::vector<DepthLevel> asks;
    Timestamp timestamp;
};

// Order location for O(1) cancel
struct OrderLocation {
    uint64_t order_id;
    Price price;
    Side side;
};

class OrderBook {
public:
    explicit OrderBook(uint64_t symbol_id);
    ~OrderBook() = default;

    // Non-copyable, non-movable (due to atomic members)
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    uint64_t symbol_id() const { return symbol_id_; }

    // Core operations - all thread-safe
    // Returns trades generated from matching
    std::vector<Trade> place_order(Order order, TradeListener* listener = nullptr);

    // Cancel order by ID, returns the cancelled order if found
    std::optional<Order> cancel_order(uint64_t order_id);

    // Modify order (cancel + replace)
    std::optional<Order> modify_order(uint64_t order_id, Price new_price, Quantity new_quantity);

    // Query operations - lock-free reads
    std::optional<Order> get_order(uint64_t order_id) const;
    bool has_order(uint64_t order_id) const;

    // Best bid/ask prices
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::optional<Price> spread() const;

    // Market depth
    MarketDepth get_depth(size_t levels = 10) const;

    // Statistics
    size_t bid_levels() const;
    size_t ask_levels() const;
    size_t total_orders() const;
    Quantity total_bid_quantity() const;
    Quantity total_ask_quantity() const;

private:
    uint64_t symbol_id_;

    // Bid side: sorted descending (highest price first)
    // Use std::greater for descending order
    std::map<Price, PriceLevel, std::greater<Price>> bids_;

    // Ask side: sorted ascending (lowest price first)
    std::map<Price, PriceLevel> asks_;

    // Order ID -> location for O(1) lookup
    std::unordered_map<uint64_t, OrderLocation> order_locations_;

    // Trade ID generator
    std::atomic<uint64_t> next_trade_id_{1};

    // Reader-writer lock for thread safety
    mutable std::shared_mutex mutex_;

    // Internal matching logic
    std::vector<Trade> match_order(Order& order, TradeListener* listener);

    template<typename BookSide>
    std::vector<Trade> match_against_side(
        Order& aggressor,
        BookSide& book_side,
        TradeListener* listener
    );

    // Check if prices cross (can match)
    bool prices_cross(Price bid_price, Price ask_price) const {
        return bid_price >= ask_price;
    }

    // Self-trade prevention check
    bool would_self_trade(const Order& a, const Order& b) const {
        return a.stp_group != 0 && a.stp_group == b.stp_group;
    }

    // Add order to resting book
    void add_to_book(Order order);

    // Remove order from book
    void remove_from_book(uint64_t order_id, Price price, Side side);

    // Generate trade record
    Trade create_trade(const Order& buy_order, const Order& sell_order,
                       Price price, Quantity quantity, Side aggressor);
};

} // namespace lux

#endif // LUX_ORDERBOOK_HPP
