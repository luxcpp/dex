#include "lux/orderbook.hpp"
#include <algorithm>
#include <cassert>
#include <set>
#include <limits>

namespace lux {

// Additional matching utilities and algorithms

// Auction matching for opening/closing auctions
struct AuctionResult {
    Price clearing_price;
    Quantity matched_volume;
    std::vector<Trade> trades;
    Quantity imbalance;  // Remaining unmatched quantity
    Side imbalance_side;
};

class AuctionMatcher {
public:
    // Calculate the clearing price that maximizes matched volume
    static AuctionResult calculate_clearing_price(
        const std::map<Price, PriceLevel, std::greater<Price>>& bids,
        const std::map<Price, PriceLevel>& asks
    ) {
        AuctionResult result{};

        if (bids.empty() || asks.empty()) {
            return result;
        }

        // Build cumulative volume curves
        std::vector<std::pair<Price, Quantity>> bid_curve;  // (price, cumulative qty at or above)
        std::vector<std::pair<Price, Quantity>> ask_curve;  // (price, cumulative qty at or below)

        Quantity cum_bid = 0;
        for (const auto& [price, level] : bids) {
            cum_bid += level.total_quantity;
            bid_curve.emplace_back(price, cum_bid);
        }

        Quantity cum_ask = 0;
        for (const auto& [price, level] : asks) {
            cum_ask += level.total_quantity;
            ask_curve.emplace_back(price, cum_ask);
        }

        // Find price that maximizes matched volume
        // Iterate through all possible prices
        std::set<Price> all_prices;
        for (const auto& [price, _] : bids) all_prices.insert(price);
        for (const auto& [price, _] : asks) all_prices.insert(price);

        Price best_price = 0;
        Quantity best_volume = 0;
        Quantity best_imbalance = std::numeric_limits<Quantity>::max();

        for (Price price : all_prices) {
            // Find bid quantity at or above this price
            Quantity bid_qty = 0;
            for (const auto& [bp, cum] : bid_curve) {
                if (bp >= price) {
                    bid_qty = cum;
                } else {
                    break;
                }
            }

            // Find ask quantity at or below this price
            Quantity ask_qty = 0;
            for (const auto& [ap, cum] : ask_curve) {
                if (ap <= price) {
                    ask_qty = cum;
                }
            }

            Quantity matched = std::min(bid_qty, ask_qty);
            Quantity imbalance = std::abs(bid_qty - ask_qty);

            // Choose price that maximizes volume, then minimizes imbalance
            if (matched > best_volume ||
                (matched == best_volume && imbalance < best_imbalance)) {
                best_price = price;
                best_volume = matched;
                best_imbalance = imbalance;
                result.imbalance = imbalance;
                result.imbalance_side = bid_qty > ask_qty ? Side::Buy : Side::Sell;
            }
        }

        result.clearing_price = best_price;
        result.matched_volume = best_volume;

        return result;
    }

    // Execute auction at clearing price
    static std::vector<Trade> execute_auction(
        std::map<Price, PriceLevel, std::greater<Price>>& bids,
        std::map<Price, PriceLevel>& asks,
        Price clearing_price,
        uint64_t symbol_id,
        std::atomic<uint64_t>& trade_id_gen
    ) {
        std::vector<Trade> trades;

        // Collect orders that will participate
        std::vector<Order*> buy_orders;
        std::vector<Order*> sell_orders;

        for (auto& [price, level] : bids) {
            if (price >= clearing_price) {
                for (auto& order : level.orders) {
                    buy_orders.push_back(&order);
                }
            }
        }

        for (auto& [price, level] : asks) {
            if (price <= clearing_price) {
                for (auto& order : level.orders) {
                    sell_orders.push_back(&order);
                }
            }
        }

        // Sort by time priority
        auto time_cmp = [](const Order* a, const Order* b) {
            return a->timestamp < b->timestamp;
        };
        std::sort(buy_orders.begin(), buy_orders.end(), time_cmp);
        std::sort(sell_orders.begin(), sell_orders.end(), time_cmp);

        // Match orders at clearing price
        size_t bi = 0, si = 0;
        while (bi < buy_orders.size() && si < sell_orders.size()) {
            Order* buy = buy_orders[bi];
            Order* sell = sell_orders[si];

            if (buy->remaining() == 0) { ++bi; continue; }
            if (sell->remaining() == 0) { ++si; continue; }

            Quantity fill_qty = std::min(buy->remaining(), sell->remaining());

            buy->filled += fill_qty;
            sell->filled += fill_qty;

            Trade trade;
            trade.id = trade_id_gen.fetch_add(1);
            trade.symbol_id = symbol_id;
            trade.buy_order_id = buy->id;
            trade.sell_order_id = sell->id;
            trade.buyer_account_id = buy->account_id;
            trade.seller_account_id = sell->account_id;
            trade.price = clearing_price;
            trade.quantity = fill_qty;
            trade.aggressor_side = Side::Buy;  // Auction has no aggressor
            trade.timestamp = std::chrono::duration_cast<Timestamp>(
                std::chrono::system_clock::now().time_since_epoch()
            );

            trades.push_back(trade);

            if (buy->remaining() == 0) ++bi;
            if (sell->remaining() == 0) ++si;
        }

        return trades;
    }
};

// Pro-rata matching (alternative to price-time priority)
class ProRataMatcher {
public:
    // Match proportionally based on order size at a price level
    static std::vector<Trade> match_pro_rata(
        Order& aggressor,
        PriceLevel& level,
        uint64_t symbol_id,
        std::atomic<uint64_t>& trade_id_gen
    ) {
        std::vector<Trade> trades;

        if (level.empty() || aggressor.remaining() == 0) {
            return trades;
        }

        Quantity aggressor_qty = aggressor.remaining();
        Quantity level_qty = level.total_quantity;

        // Calculate each order's share
        std::vector<std::pair<Order*, Quantity>> fills;

        for (auto& order : level.orders) {
            // Pro-rata allocation: order_qty / level_qty * aggressor_qty
            // Use integer math to avoid floating point issues
            Quantity share = (order.remaining() * aggressor_qty) / level_qty;
            if (share > 0) {
                fills.emplace_back(&order, share);
            }
        }

        // Distribute any remaining quantity due to rounding (FIFO)
        Quantity allocated = 0;
        for (const auto& [_, qty] : fills) {
            allocated += qty;
        }

        Quantity remainder = std::min(aggressor_qty, level_qty) - allocated;
        for (auto& [order, qty] : fills) {
            if (remainder == 0) break;
            Quantity extra = std::min(remainder, order->remaining() - qty);
            qty += extra;
            remainder -= extra;
        }

        // Execute fills
        for (auto& [order, fill_qty] : fills) {
            if (fill_qty == 0) continue;

            // Cap at available quantities
            fill_qty = std::min(fill_qty, std::min(aggressor.remaining(), order->remaining()));

            aggressor.filled += fill_qty;
            order->filled += fill_qty;

            Trade trade;
            trade.id = trade_id_gen.fetch_add(1);
            trade.symbol_id = symbol_id;
            trade.price = level.price;
            trade.quantity = fill_qty;
            trade.timestamp = std::chrono::duration_cast<Timestamp>(
                std::chrono::system_clock::now().time_since_epoch()
            );

            if (aggressor.is_buy()) {
                trade.buy_order_id = aggressor.id;
                trade.sell_order_id = order->id;
                trade.buyer_account_id = aggressor.account_id;
                trade.seller_account_id = order->account_id;
            } else {
                trade.buy_order_id = order->id;
                trade.sell_order_id = aggressor.id;
                trade.buyer_account_id = order->account_id;
                trade.seller_account_id = aggressor.account_id;
            }
            trade.aggressor_side = aggressor.side;

            trades.push_back(trade);
        }

        // Remove filled orders
        level.orders.remove_if([](const Order& o) { return o.is_filled(); });

        // Recalculate total quantity
        level.total_quantity = 0;
        for (const auto& order : level.orders) {
            level.total_quantity += order.remaining();
        }

        return trades;
    }
};

// Iceberg order support (hidden quantity)
struct IcebergOrder {
    Order visible_order;
    Quantity total_quantity;
    Quantity display_quantity;
    Quantity hidden_remaining;

    bool has_hidden() const { return hidden_remaining > 0; }

    void replenish() {
        if (hidden_remaining > 0) {
            Quantity replenish_qty = std::min(display_quantity, hidden_remaining);
            visible_order.quantity = visible_order.filled + replenish_qty;
            hidden_remaining -= replenish_qty;
        }
    }
};

// Stop order book (orders waiting for trigger)
class StopOrderBook {
public:
    void add_stop_order(Order order) {
        if (order.is_buy()) {
            // Buy stop triggers when price rises above stop price
            buy_stops_[order.stop_price].push_back(std::move(order));
        } else {
            // Sell stop triggers when price falls below stop price
            sell_stops_[order.stop_price].push_back(std::move(order));
        }
    }

    // Check and return triggered orders
    std::vector<Order> check_triggers(Price last_price, Price prev_price) {
        std::vector<Order> triggered;

        // Buy stops trigger on uptick
        if (last_price > prev_price) {
            auto it = buy_stops_.begin();
            while (it != buy_stops_.end() && it->first <= last_price) {
                for (auto& order : it->second) {
                    // Convert to market or limit order
                    if (order.type == OrderType::Stop) {
                        order.type = OrderType::Market;
                    } else {  // StopLimit
                        order.type = OrderType::Limit;
                    }
                    triggered.push_back(std::move(order));
                }
                it = buy_stops_.erase(it);
            }
        }

        // Sell stops trigger on downtick
        if (last_price < prev_price) {
            auto it = sell_stops_.rbegin();
            while (it != sell_stops_.rend() && it->first >= last_price) {
                for (auto& order : it->second) {
                    if (order.type == OrderType::Stop) {
                        order.type = OrderType::Market;
                    } else {
                        order.type = OrderType::Limit;
                    }
                    triggered.push_back(std::move(order));
                }
                sell_stops_.erase(std::next(it).base());
                it = sell_stops_.rbegin();
            }
        }

        return triggered;
    }

    bool cancel_stop(uint64_t order_id) {
        for (auto& [_, orders] : buy_stops_) {
            auto it = std::find_if(orders.begin(), orders.end(),
                [order_id](const Order& o) { return o.id == order_id; });
            if (it != orders.end()) {
                orders.erase(it);
                return true;
            }
        }

        for (auto& [_, orders] : sell_stops_) {
            auto it = std::find_if(orders.begin(), orders.end(),
                [order_id](const Order& o) { return o.id == order_id; });
            if (it != orders.end()) {
                orders.erase(it);
                return true;
            }
        }

        return false;
    }

private:
    // Buy stops: trigger when price >= stop_price (sorted ascending)
    std::map<Price, std::vector<Order>> buy_stops_;

    // Sell stops: trigger when price <= stop_price (sorted descending for efficient iteration)
    std::map<Price, std::vector<Order>, std::greater<Price>> sell_stops_;
};

} // namespace lux
