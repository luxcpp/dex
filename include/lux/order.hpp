#ifndef LUX_ORDER_HPP
#define LUX_ORDER_HPP

#include <cstdint>
#include <chrono>
#include <string>

namespace lux {

enum class Side : uint8_t {
    Buy = 0,
    Sell = 1
};

enum class OrderType : uint8_t {
    Limit = 0,
    Market = 1,
    Stop = 2,
    StopLimit = 3
};

enum class TimeInForce : uint8_t {
    GTC = 0,  // Good Till Cancel
    IOC = 1,  // Immediate Or Cancel
    FOK = 2,  // Fill Or Kill
    GTD = 3,  // Good Till Date
    DAY = 4   // Day order
};

enum class OrderStatus : uint8_t {
    New = 0,
    PartiallyFilled = 1,
    Filled = 2,
    Cancelled = 3,
    Rejected = 4,
    Expired = 5
};

using Timestamp = std::chrono::nanoseconds;
using Price = int64_t;      // Fixed-point: actual_price * 1e8
using Quantity = int64_t;   // Fixed-point: actual_qty * 1e8

constexpr int64_t PRICE_MULTIPLIER = 100000000LL;  // 1e8

struct Order {
    uint64_t id;
    uint64_t symbol_id;
    uint64_t account_id;

    Price price;
    Quantity quantity;
    Quantity filled;
    Quantity remaining() const { return quantity - filled; }

    Side side;
    OrderType type;
    TimeInForce tif;
    OrderStatus status;

    Timestamp timestamp;
    Timestamp expire_time;

    // Self-trade prevention group (orders with same STP group won't match)
    uint64_t stp_group;

    // For stop orders
    Price stop_price;

    bool is_buy() const { return side == Side::Buy; }
    bool is_sell() const { return side == Side::Sell; }
    bool is_active() const {
        return status == OrderStatus::New || status == OrderStatus::PartiallyFilled;
    }
    bool is_filled() const { return remaining() == 0; }

    // Convert from double to fixed-point
    static Price to_price(double p) {
        return static_cast<Price>(p * PRICE_MULTIPLIER);
    }

    static Quantity to_quantity(double q) {
        return static_cast<Quantity>(q * PRICE_MULTIPLIER);
    }

    // Convert from fixed-point to double
    static double from_price(Price p) {
        return static_cast<double>(p) / PRICE_MULTIPLIER;
    }

    static double from_quantity(Quantity q) {
        return static_cast<double>(q) / PRICE_MULTIPLIER;
    }
};

// Order creation helper
struct OrderBuilder {
    Order order{};

    OrderBuilder& id(uint64_t v) { order.id = v; return *this; }
    OrderBuilder& symbol(uint64_t v) { order.symbol_id = v; return *this; }
    OrderBuilder& account(uint64_t v) { order.account_id = v; return *this; }
    OrderBuilder& price(double v) { order.price = Order::to_price(v); return *this; }
    OrderBuilder& quantity(double v) { order.quantity = Order::to_quantity(v); return *this; }
    OrderBuilder& side(Side v) { order.side = v; return *this; }
    OrderBuilder& type(OrderType v) { order.type = v; return *this; }
    OrderBuilder& tif(TimeInForce v) { order.tif = v; return *this; }
    OrderBuilder& stp_group(uint64_t v) { order.stp_group = v; return *this; }
    OrderBuilder& stop_price(double v) { order.stop_price = Order::to_price(v); return *this; }

    Order build() {
        order.filled = 0;
        order.status = OrderStatus::New;
        order.timestamp = std::chrono::duration_cast<Timestamp>(
            std::chrono::system_clock::now().time_since_epoch()
        );
        return order;
    }
};

} // namespace lux

#endif // LUX_ORDER_HPP
