#ifndef LUX_TRADE_HPP
#define LUX_TRADE_HPP

#include <cstdint>
#include <chrono>
#include "order.hpp"

namespace lux {

struct Trade {
    uint64_t id;
    uint64_t symbol_id;

    uint64_t buy_order_id;
    uint64_t sell_order_id;
    uint64_t buyer_account_id;
    uint64_t seller_account_id;

    Price price;
    Quantity quantity;

    Timestamp timestamp;

    // Which side was the aggressor (taker)
    Side aggressor_side;

    double price_double() const { return Order::from_price(price); }
    double quantity_double() const { return Order::from_quantity(quantity); }
};

// Callback interface for trade notifications
class TradeListener {
public:
    virtual ~TradeListener() = default;
    virtual void on_trade(const Trade& trade) = 0;
    virtual void on_order_filled(const Order& order) = 0;
    virtual void on_order_partially_filled(const Order& order, Quantity fill_qty) = 0;
    virtual void on_order_cancelled(const Order& order) = 0;
};

// No-op listener for when notifications aren't needed
class NullTradeListener : public TradeListener {
public:
    void on_trade(const Trade&) override {}
    void on_order_filled(const Order&) override {}
    void on_order_partially_filled(const Order&, Quantity) override {}
    void on_order_cancelled(const Order&) override {}
};

} // namespace lux

#endif // LUX_TRADE_HPP
