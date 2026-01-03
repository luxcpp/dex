#include "luxdex_c.h"
#include "lux/engine.hpp"
#include <cstring>
#include <new>

// Convert C order to C++ order
static lux::Order to_cpp_order(const LuxOrder* order) {
    lux::Order o;
    o.id = order->id;
    o.symbol_id = order->symbol_id;
    o.account_id = order->account_id;
    o.price = order->price;
    o.quantity = order->quantity;
    o.filled = order->filled;
    o.side = static_cast<lux::Side>(order->side);
    o.type = static_cast<lux::OrderType>(order->order_type);
    o.tif = static_cast<lux::TimeInForce>(order->tif);
    o.status = static_cast<lux::OrderStatus>(order->status);
    o.stp_group = order->stp_group;
    o.stop_price = order->stop_price;
    o.timestamp = lux::Timestamp(order->timestamp_ns);
    return o;
}

// Convert C++ order to C order
static void to_c_order(const lux::Order& order, LuxOrder* out) {
    out->id = order.id;
    out->symbol_id = order.symbol_id;
    out->account_id = order.account_id;
    out->price = order.price;
    out->quantity = order.quantity;
    out->filled = order.filled;
    out->side = static_cast<LuxSide>(order.side);
    out->order_type = static_cast<LuxOrderType>(order.type);
    out->tif = static_cast<LuxTimeInForce>(order.tif);
    out->status = static_cast<LuxOrderStatus>(order.status);
    out->stp_group = order.stp_group;
    out->stop_price = order.stop_price;
    out->timestamp_ns = order.timestamp.count();
}

// Convert C++ trade to C trade
static void to_c_trade(const lux::Trade& trade, LuxTrade* out) {
    out->id = trade.id;
    out->symbol_id = trade.symbol_id;
    out->buy_order_id = trade.buy_order_id;
    out->sell_order_id = trade.sell_order_id;
    out->buyer_account_id = trade.buyer_account_id;
    out->seller_account_id = trade.seller_account_id;
    out->price = trade.price;
    out->quantity = trade.quantity;
    out->aggressor_side = static_cast<LuxSide>(trade.aggressor_side);
    out->timestamp_ns = trade.timestamp.count();
}

extern "C" {

// =============================================================================
// Engine API
// =============================================================================

LuxEngine lux_engine_create(void) {
    try {
        return new lux::Engine();
    } catch (...) {
        return nullptr;
    }
}

LuxEngine lux_engine_create_with_config(const LuxEngineConfig* config) {
    if (!config) return lux_engine_create();

    try {
        lux::EngineConfig cfg;
        cfg.worker_threads = config->worker_threads;
        cfg.max_batch_size = config->max_batch_size;
        cfg.enable_self_trade_prevention = config->enable_stp;
        cfg.async_mode = config->async_mode;
        return new lux::Engine(cfg);
    } catch (...) {
        return nullptr;
    }
}

void lux_engine_destroy(LuxEngine engine) {
    delete static_cast<lux::Engine*>(engine);
}

void lux_engine_start(LuxEngine engine) {
    if (engine) {
        static_cast<lux::Engine*>(engine)->start();
    }
}

void lux_engine_stop(LuxEngine engine) {
    if (engine) {
        static_cast<lux::Engine*>(engine)->stop();
    }
}

bool lux_engine_is_running(LuxEngine engine) {
    return engine && static_cast<lux::Engine*>(engine)->is_running();
}

bool lux_engine_add_symbol(LuxEngine engine, uint64_t symbol_id) {
    if (!engine) return false;
    return static_cast<lux::Engine*>(engine)->add_symbol(symbol_id);
}

bool lux_engine_remove_symbol(LuxEngine engine, uint64_t symbol_id) {
    if (!engine) return false;
    return static_cast<lux::Engine*>(engine)->remove_symbol(symbol_id);
}

bool lux_engine_has_symbol(LuxEngine engine, uint64_t symbol_id) {
    if (!engine) return false;
    return static_cast<lux::Engine*>(engine)->has_symbol(symbol_id);
}

uint64_t* lux_engine_symbols(LuxEngine engine, size_t* count) {
    if (!engine || !count) {
        if (count) *count = 0;
        return nullptr;
    }

    auto symbols = static_cast<lux::Engine*>(engine)->symbols();
    *count = symbols.size();

    if (symbols.empty()) return nullptr;

    uint64_t* result = new(std::nothrow) uint64_t[symbols.size()];
    if (!result) {
        *count = 0;
        return nullptr;
    }

    std::memcpy(result, symbols.data(), symbols.size() * sizeof(uint64_t));
    return result;
}

LuxOrderResult lux_engine_place_order(LuxEngine engine, const LuxOrder* order) {
    LuxOrderResult result{};

    if (!engine || !order) {
        result.success = false;
        std::strncpy(result.error, "Invalid engine or order", sizeof(result.error) - 1);
        return result;
    }

    lux::Order cpp_order = to_cpp_order(order);
    auto cpp_result = static_cast<lux::Engine*>(engine)->place_order(cpp_order);

    result.success = cpp_result.success;
    result.order_id = cpp_result.order_id;

    if (!cpp_result.error.empty()) {
        std::strncpy(result.error, cpp_result.error.c_str(), sizeof(result.error) - 1);
    }

    result.trade_count = cpp_result.trades.size();
    if (result.trade_count > 0) {
        result.trades = new(std::nothrow) LuxTrade[result.trade_count];
        if (result.trades) {
            for (size_t i = 0; i < result.trade_count; ++i) {
                to_c_trade(cpp_result.trades[i], &result.trades[i]);
            }
        } else {
            result.trade_count = 0;
        }
    }

    return result;
}

LuxCancelResult lux_engine_cancel_order(LuxEngine engine, uint64_t symbol_id, uint64_t order_id) {
    LuxCancelResult result{};

    if (!engine) {
        result.success = false;
        std::strncpy(result.error, "Invalid engine", sizeof(result.error) - 1);
        return result;
    }

    auto cpp_result = static_cast<lux::Engine*>(engine)->cancel_order(symbol_id, order_id);

    result.success = cpp_result.success;
    result.has_order = cpp_result.cancelled_order.has_value();

    if (result.has_order) {
        to_c_order(*cpp_result.cancelled_order, &result.cancelled_order);
    }

    if (!cpp_result.error.empty()) {
        std::strncpy(result.error, cpp_result.error.c_str(), sizeof(result.error) - 1);
    }

    return result;
}

bool lux_engine_get_order(LuxEngine engine, uint64_t symbol_id, uint64_t order_id, LuxOrder* out) {
    if (!engine || !out) return false;

    auto order = static_cast<lux::Engine*>(engine)->get_order(symbol_id, order_id);
    if (!order) return false;

    to_c_order(*order, out);
    return true;
}

LuxMarketDepth lux_engine_get_depth(LuxEngine engine, uint64_t symbol_id, size_t levels) {
    LuxMarketDepth result{};

    if (!engine) return result;

    auto depth = static_cast<lux::Engine*>(engine)->get_depth(symbol_id, levels);

    result.timestamp_ns = depth.timestamp.count();

    result.bid_count = depth.bids.size();
    if (result.bid_count > 0) {
        result.bids = new(std::nothrow) LuxDepthLevel[result.bid_count];
        if (result.bids) {
            for (size_t i = 0; i < result.bid_count; ++i) {
                result.bids[i].price = depth.bids[i].price;
                result.bids[i].quantity = depth.bids[i].quantity;
                result.bids[i].order_count = depth.bids[i].order_count;
            }
        } else {
            result.bid_count = 0;
        }
    }

    result.ask_count = depth.asks.size();
    if (result.ask_count > 0) {
        result.asks = new(std::nothrow) LuxDepthLevel[result.ask_count];
        if (result.asks) {
            for (size_t i = 0; i < result.ask_count; ++i) {
                result.asks[i].price = depth.asks[i].price;
                result.asks[i].quantity = depth.asks[i].quantity;
                result.asks[i].order_count = depth.asks[i].order_count;
            }
        } else {
            result.ask_count = 0;
        }
    }

    return result;
}

bool lux_engine_best_bid(LuxEngine engine, uint64_t symbol_id, LuxPrice* price) {
    if (!engine || !price) return false;

    auto bid = static_cast<lux::Engine*>(engine)->best_bid(symbol_id);
    if (!bid) return false;

    *price = *bid;
    return true;
}

bool lux_engine_best_ask(LuxEngine engine, uint64_t symbol_id, LuxPrice* price) {
    if (!engine || !price) return false;

    auto ask = static_cast<lux::Engine*>(engine)->best_ask(symbol_id);
    if (!ask) return false;

    *price = *ask;
    return true;
}

LuxEngineStats lux_engine_get_stats(LuxEngine engine) {
    LuxEngineStats result{};

    if (!engine) return result;

    auto stats = static_cast<lux::Engine*>(engine)->get_stats();
    result.total_orders_placed = stats.total_orders_placed;
    result.total_orders_cancelled = stats.total_orders_cancelled;
    result.total_trades = stats.total_trades;
    result.total_volume = stats.total_volume;

    return result;
}

// =============================================================================
// OrderBook API
// =============================================================================

LuxOrderBook lux_engine_get_orderbook(LuxEngine engine, uint64_t symbol_id) {
    if (!engine) return nullptr;
    return static_cast<lux::Engine*>(engine)->get_orderbook(symbol_id);
}

LuxOrderResult lux_orderbook_place_order(LuxOrderBook book, const LuxOrder* order) {
    LuxOrderResult result{};

    if (!book || !order) {
        result.success = false;
        std::strncpy(result.error, "Invalid orderbook or order", sizeof(result.error) - 1);
        return result;
    }

    lux::Order cpp_order = to_cpp_order(order);

    try {
        auto trades = static_cast<lux::OrderBook*>(book)->place_order(cpp_order);

        result.success = true;
        result.order_id = cpp_order.id;
        result.trade_count = trades.size();

        if (result.trade_count > 0) {
            result.trades = new(std::nothrow) LuxTrade[result.trade_count];
            if (result.trades) {
                for (size_t i = 0; i < result.trade_count; ++i) {
                    to_c_trade(trades[i], &result.trades[i]);
                }
            } else {
                result.trade_count = 0;
            }
        }
    } catch (const std::exception& e) {
        result.success = false;
        std::strncpy(result.error, e.what(), sizeof(result.error) - 1);
    }

    return result;
}

LuxCancelResult lux_orderbook_cancel_order(LuxOrderBook book, uint64_t order_id) {
    LuxCancelResult result{};

    if (!book) {
        result.success = false;
        std::strncpy(result.error, "Invalid orderbook", sizeof(result.error) - 1);
        return result;
    }

    auto cancelled = static_cast<lux::OrderBook*>(book)->cancel_order(order_id);

    result.success = cancelled.has_value();
    result.has_order = cancelled.has_value();

    if (cancelled) {
        to_c_order(*cancelled, &result.cancelled_order);
    } else {
        std::strncpy(result.error, "Order not found", sizeof(result.error) - 1);
    }

    return result;
}

bool lux_orderbook_get_order(LuxOrderBook book, uint64_t order_id, LuxOrder* out) {
    if (!book || !out) return false;

    auto order = static_cast<lux::OrderBook*>(book)->get_order(order_id);
    if (!order) return false;

    to_c_order(*order, out);
    return true;
}

LuxMarketDepth lux_orderbook_get_depth(LuxOrderBook book, size_t levels) {
    LuxMarketDepth result{};

    if (!book) return result;

    auto depth = static_cast<lux::OrderBook*>(book)->get_depth(levels);

    result.timestamp_ns = depth.timestamp.count();

    result.bid_count = depth.bids.size();
    if (result.bid_count > 0) {
        result.bids = new(std::nothrow) LuxDepthLevel[result.bid_count];
        if (result.bids) {
            for (size_t i = 0; i < result.bid_count; ++i) {
                result.bids[i].price = depth.bids[i].price;
                result.bids[i].quantity = depth.bids[i].quantity;
                result.bids[i].order_count = depth.bids[i].order_count;
            }
        } else {
            result.bid_count = 0;
        }
    }

    result.ask_count = depth.asks.size();
    if (result.ask_count > 0) {
        result.asks = new(std::nothrow) LuxDepthLevel[result.ask_count];
        if (result.asks) {
            for (size_t i = 0; i < result.ask_count; ++i) {
                result.asks[i].price = depth.asks[i].price;
                result.asks[i].quantity = depth.asks[i].quantity;
                result.asks[i].order_count = depth.asks[i].order_count;
            }
        } else {
            result.ask_count = 0;
        }
    }

    return result;
}

size_t lux_orderbook_bid_levels(LuxOrderBook book) {
    if (!book) return 0;
    return static_cast<lux::OrderBook*>(book)->bid_levels();
}

size_t lux_orderbook_ask_levels(LuxOrderBook book) {
    if (!book) return 0;
    return static_cast<lux::OrderBook*>(book)->ask_levels();
}

size_t lux_orderbook_total_orders(LuxOrderBook book) {
    if (!book) return 0;
    return static_cast<lux::OrderBook*>(book)->total_orders();
}

// =============================================================================
// Memory management
// =============================================================================

void lux_order_result_free(LuxOrderResult* result) {
    if (result && result->trades) {
        delete[] result->trades;
        result->trades = nullptr;
        result->trade_count = 0;
    }
}

void lux_market_depth_free(LuxMarketDepth* depth) {
    if (depth) {
        delete[] depth->bids;
        delete[] depth->asks;
        depth->bids = nullptr;
        depth->asks = nullptr;
        depth->bid_count = 0;
        depth->ask_count = 0;
    }
}

void lux_symbols_free(uint64_t* symbols) {
    delete[] symbols;
}

// =============================================================================
// Utility
// =============================================================================

uint64_t lux_generate_order_id(void) {
    return lux::OrderIdGenerator::instance().next();
}

void lux_reset_order_id_generator(uint64_t start) {
    lux::OrderIdGenerator::instance().reset(start);
}

} // extern "C"
