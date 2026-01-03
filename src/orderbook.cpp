#include "lux/orderbook.hpp"
#include <algorithm>
#include <stdexcept>

namespace lux {

OrderBook::OrderBook(uint64_t symbol_id)
    : symbol_id_(symbol_id) {}

std::vector<Trade> OrderBook::place_order(Order order, TradeListener* listener) {
    std::unique_lock lock(mutex_);

    // Validate order
    if (order.quantity <= 0) {
        throw std::invalid_argument("Order quantity must be positive");
    }

    if (order.type == OrderType::Limit && order.price <= 0) {
        throw std::invalid_argument("Limit order price must be positive");
    }

    order.status = OrderStatus::New;
    order.filled = 0;
    order.symbol_id = symbol_id_;

    // Set timestamp if not already set
    if (order.timestamp.count() == 0) {
        order.timestamp = std::chrono::duration_cast<Timestamp>(
            std::chrono::system_clock::now().time_since_epoch()
        );
    }

    std::vector<Trade> trades;

    // Market orders and limit orders get matched
    if (order.type == OrderType::Market || order.type == OrderType::Limit) {
        trades = match_order(order, listener);
    }

    // Handle remaining quantity based on TimeInForce
    if (order.remaining() > 0) {
        switch (order.tif) {
            case TimeInForce::IOC:
                // Immediate or Cancel: cancel remaining
                order.status = order.filled > 0 ?
                    OrderStatus::PartiallyFilled : OrderStatus::Cancelled;
                if (listener) {
                    listener->on_order_cancelled(order);
                }
                break;

            case TimeInForce::FOK:
                // Fill or Kill: should have been fully filled or rejected
                // If we get here with remaining, the order was rejected
                order.status = OrderStatus::Rejected;
                break;

            case TimeInForce::GTC:
            case TimeInForce::GTD:
            case TimeInForce::DAY:
                // Add to book if limit order
                if (order.type == OrderType::Limit) {
                    add_to_book(order);
                } else {
                    // Market orders that couldn't be fully filled
                    order.status = order.filled > 0 ?
                        OrderStatus::PartiallyFilled : OrderStatus::Cancelled;
                }
                break;
        }
    } else {
        order.status = OrderStatus::Filled;
        if (listener) {
            listener->on_order_filled(order);
        }
    }

    return trades;
}

std::vector<Trade> OrderBook::match_order(Order& order, TradeListener* listener) {
    std::vector<Trade> trades;

    // FOK check: ensure we can fill the entire order
    if (order.tif == TimeInForce::FOK) {
        Quantity available = 0;
        if (order.is_buy()) {
            for (const auto& [price, level] : asks_) {
                if (order.type == OrderType::Market ||
                    prices_cross(order.price, price)) {
                    available += level.total_quantity;
                    if (available >= order.quantity) break;
                } else {
                    break;
                }
            }
        } else {
            for (const auto& [price, level] : bids_) {
                if (order.type == OrderType::Market ||
                    prices_cross(price, order.price)) {
                    available += level.total_quantity;
                    if (available >= order.quantity) break;
                } else {
                    break;
                }
            }
        }

        if (available < order.quantity) {
            order.status = OrderStatus::Rejected;
            return trades;
        }
    }

    // Match against opposite side
    if (order.is_buy()) {
        trades = match_against_side(order, asks_, listener);
    } else {
        trades = match_against_side(order, bids_, listener);
    }

    return trades;
}

template<typename BookSide>
std::vector<Trade> OrderBook::match_against_side(
    Order& aggressor,
    BookSide& book_side,
    TradeListener* listener
) {
    std::vector<Trade> trades;

    auto it = book_side.begin();
    while (it != book_side.end() && aggressor.remaining() > 0) {
        PriceLevel& level = it->second;
        Price level_price = it->first;

        // Check if prices cross
        bool crosses;
        if (aggressor.type == OrderType::Market) {
            crosses = true;
        } else if (aggressor.is_buy()) {
            crosses = prices_cross(aggressor.price, level_price);
        } else {
            crosses = prices_cross(level_price, aggressor.price);
        }

        if (!crosses) {
            break;
        }

        // Match against orders at this price level (FIFO)
        while (!level.empty() && aggressor.remaining() > 0) {
            Order* resting = level.front();

            // Self-trade prevention
            if (would_self_trade(aggressor, *resting)) {
                // Cancel the resting order
                Order cancelled = *resting;
                cancelled.status = OrderStatus::Cancelled;
                level.pop_front();
                order_locations_.erase(cancelled.id);
                if (listener) {
                    listener->on_order_cancelled(cancelled);
                }
                continue;
            }

            // Calculate fill quantity
            Quantity fill_qty = std::min(aggressor.remaining(), resting->remaining());

            // Update orders
            aggressor.filled += fill_qty;
            resting->filled += fill_qty;

            // Create trade
            Trade trade = aggressor.is_buy() ?
                create_trade(aggressor, *resting, level_price, fill_qty, aggressor.side) :
                create_trade(*resting, aggressor, level_price, fill_qty, aggressor.side);

            trades.push_back(trade);

            if (listener) {
                listener->on_trade(trade);

                if (aggressor.is_filled()) {
                    listener->on_order_filled(aggressor);
                } else {
                    listener->on_order_partially_filled(aggressor, fill_qty);
                }

                if (resting->is_filled()) {
                    listener->on_order_filled(*resting);
                } else {
                    listener->on_order_partially_filled(*resting, fill_qty);
                }
            }

            // Remove filled resting order
            if (resting->is_filled()) {
                order_locations_.erase(resting->id);
                level.pop_front();
            }
        }

        // Remove empty price level
        if (level.empty()) {
            it = book_side.erase(it);
        } else {
            ++it;
        }
    }

    // Update aggressor status
    if (aggressor.filled > 0) {
        aggressor.status = aggressor.is_filled() ?
            OrderStatus::Filled : OrderStatus::PartiallyFilled;
    }

    return trades;
}

// Explicit template instantiations
template std::vector<Trade> OrderBook::match_against_side(
    Order&, std::map<Price, PriceLevel, std::greater<Price>>&, TradeListener*);
template std::vector<Trade> OrderBook::match_against_side(
    Order&, std::map<Price, PriceLevel>&, TradeListener*);

void OrderBook::add_to_book(Order order) {
    order.status = order.filled > 0 ?
        OrderStatus::PartiallyFilled : OrderStatus::New;

    OrderLocation loc{order.id, order.price, order.side};
    order_locations_[order.id] = loc;

    if (order.is_buy()) {
        auto& level = bids_[order.price];
        level.price = order.price;
        level.add_order(std::move(order));
    } else {
        auto& level = asks_[order.price];
        level.price = order.price;
        level.add_order(std::move(order));
    }
}

void OrderBook::remove_from_book(uint64_t order_id, Price price, Side side) {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        if (it != bids_.end()) {
            it->second.remove_order(order_id);
            if (it->second.empty()) {
                bids_.erase(it);
            }
        }
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end()) {
            it->second.remove_order(order_id);
            if (it->second.empty()) {
                asks_.erase(it);
            }
        }
    }
    order_locations_.erase(order_id);
}

std::optional<Order> OrderBook::cancel_order(uint64_t order_id) {
    std::unique_lock lock(mutex_);

    auto loc_it = order_locations_.find(order_id);
    if (loc_it == order_locations_.end()) {
        return std::nullopt;
    }

    OrderLocation loc = loc_it->second;
    std::optional<Order> cancelled;

    if (loc.side == Side::Buy) {
        auto level_it = bids_.find(loc.price);
        if (level_it != bids_.end()) {
            for (auto& order : level_it->second.orders) {
                if (order.id == order_id) {
                    order.status = OrderStatus::Cancelled;
                    cancelled = order;
                    break;
                }
            }
            level_it->second.remove_order(order_id);
            if (level_it->second.empty()) {
                bids_.erase(level_it);
            }
        }
    } else {
        auto level_it = asks_.find(loc.price);
        if (level_it != asks_.end()) {
            for (auto& order : level_it->second.orders) {
                if (order.id == order_id) {
                    order.status = OrderStatus::Cancelled;
                    cancelled = order;
                    break;
                }
            }
            level_it->second.remove_order(order_id);
            if (level_it->second.empty()) {
                asks_.erase(level_it);
            }
        }
    }

    order_locations_.erase(loc_it);
    return cancelled;
}

std::optional<Order> OrderBook::modify_order(uint64_t order_id, Price new_price, Quantity new_quantity) {
    std::unique_lock lock(mutex_);

    auto loc_it = order_locations_.find(order_id);
    if (loc_it == order_locations_.end()) {
        return std::nullopt;
    }

    OrderLocation loc = loc_it->second;

    // Find and copy the original order
    Order original;
    bool found = false;

    if (loc.side == Side::Buy) {
        auto level_it = bids_.find(loc.price);
        if (level_it != bids_.end()) {
            for (const auto& order : level_it->second.orders) {
                if (order.id == order_id) {
                    original = order;
                    found = true;
                    break;
                }
            }
        }
    } else {
        auto level_it = asks_.find(loc.price);
        if (level_it != asks_.end()) {
            for (const auto& order : level_it->second.orders) {
                if (order.id == order_id) {
                    original = order;
                    found = true;
                    break;
                }
            }
        }
    }

    if (!found) {
        return std::nullopt;
    }

    // Remove old order
    remove_from_book(order_id, loc.price, loc.side);

    // Create modified order
    Order modified = original;
    modified.price = new_price;
    modified.quantity = new_quantity;
    modified.timestamp = std::chrono::duration_cast<Timestamp>(
        std::chrono::system_clock::now().time_since_epoch()
    );

    // Validate new quantity
    if (new_quantity <= modified.filled) {
        modified.status = OrderStatus::Cancelled;
        return modified;
    }

    // Add back to book
    add_to_book(modified);
    return modified;
}

std::optional<Order> OrderBook::get_order(uint64_t order_id) const {
    std::shared_lock lock(mutex_);

    auto loc_it = order_locations_.find(order_id);
    if (loc_it == order_locations_.end()) {
        return std::nullopt;
    }

    const OrderLocation& loc = loc_it->second;

    if (loc.side == Side::Buy) {
        auto level_it = bids_.find(loc.price);
        if (level_it != bids_.end()) {
            for (const auto& order : level_it->second.orders) {
                if (order.id == order_id) {
                    return order;
                }
            }
        }
    } else {
        auto level_it = asks_.find(loc.price);
        if (level_it != asks_.end()) {
            for (const auto& order : level_it->second.orders) {
                if (order.id == order_id) {
                    return order;
                }
            }
        }
    }

    return std::nullopt;
}

bool OrderBook::has_order(uint64_t order_id) const {
    std::shared_lock lock(mutex_);
    return order_locations_.find(order_id) != order_locations_.end();
}

std::optional<Price> OrderBook::best_bid() const {
    std::shared_lock lock(mutex_);
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    std::shared_lock lock(mutex_);
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<Price> OrderBook::spread() const {
    auto bid = best_bid();
    auto ask = best_ask();
    if (!bid || !ask) return std::nullopt;
    return *ask - *bid;
}

MarketDepth OrderBook::get_depth(size_t levels) const {
    std::shared_lock lock(mutex_);

    MarketDepth depth;
    depth.timestamp = std::chrono::duration_cast<Timestamp>(
        std::chrono::system_clock::now().time_since_epoch()
    );

    // Bids (already sorted descending)
    size_t count = 0;
    for (const auto& [price, level] : bids_) {
        if (count >= levels) break;
        depth.bids.push_back({
            Order::from_price(price),
            Order::from_quantity(level.total_quantity),
            static_cast<int>(level.order_count())
        });
        ++count;
    }

    // Asks (already sorted ascending)
    count = 0;
    for (const auto& [price, level] : asks_) {
        if (count >= levels) break;
        depth.asks.push_back({
            Order::from_price(price),
            Order::from_quantity(level.total_quantity),
            static_cast<int>(level.order_count())
        });
        ++count;
    }

    return depth;
}

size_t OrderBook::bid_levels() const {
    std::shared_lock lock(mutex_);
    return bids_.size();
}

size_t OrderBook::ask_levels() const {
    std::shared_lock lock(mutex_);
    return asks_.size();
}

size_t OrderBook::total_orders() const {
    std::shared_lock lock(mutex_);
    return order_locations_.size();
}

Quantity OrderBook::total_bid_quantity() const {
    std::shared_lock lock(mutex_);
    Quantity total = 0;
    for (const auto& [_, level] : bids_) {
        total += level.total_quantity;
    }
    return total;
}

Quantity OrderBook::total_ask_quantity() const {
    std::shared_lock lock(mutex_);
    Quantity total = 0;
    for (const auto& [_, level] : asks_) {
        total += level.total_quantity;
    }
    return total;
}

Trade OrderBook::create_trade(
    const Order& buy_order,
    const Order& sell_order,
    Price price,
    Quantity quantity,
    Side aggressor
) {
    Trade trade;
    trade.id = next_trade_id_.fetch_add(1, std::memory_order_relaxed);
    trade.symbol_id = symbol_id_;
    trade.buy_order_id = buy_order.id;
    trade.sell_order_id = sell_order.id;
    trade.buyer_account_id = buy_order.account_id;
    trade.seller_account_id = sell_order.account_id;
    trade.price = price;
    trade.quantity = quantity;
    trade.aggressor_side = aggressor;
    trade.timestamp = std::chrono::duration_cast<Timestamp>(
        std::chrono::system_clock::now().time_since_epoch()
    );
    return trade;
}

} // namespace lux
