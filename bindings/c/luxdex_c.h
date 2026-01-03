#ifndef LUXDEX_C_H
#define LUXDEX_C_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef void* LuxEngine;
typedef void* LuxOrderBook;

// Enums
typedef enum {
    LUX_SIDE_BUY = 0,
    LUX_SIDE_SELL = 1
} LuxSide;

typedef enum {
    LUX_ORDER_LIMIT = 0,
    LUX_ORDER_MARKET = 1,
    LUX_ORDER_STOP = 2,
    LUX_ORDER_STOP_LIMIT = 3
} LuxOrderType;

typedef enum {
    LUX_TIF_GTC = 0,
    LUX_TIF_IOC = 1,
    LUX_TIF_FOK = 2,
    LUX_TIF_GTD = 3,
    LUX_TIF_DAY = 4
} LuxTimeInForce;

typedef enum {
    LUX_STATUS_NEW = 0,
    LUX_STATUS_PARTIAL = 1,
    LUX_STATUS_FILLED = 2,
    LUX_STATUS_CANCELLED = 3,
    LUX_STATUS_REJECTED = 4,
    LUX_STATUS_EXPIRED = 5
} LuxOrderStatus;

// Fixed-point price/quantity (actual_value * 1e8)
typedef int64_t LuxPrice;
typedef int64_t LuxQuantity;

#define LUX_PRICE_MULTIPLIER 100000000LL

// Helper macros for price/quantity conversion
#define LUX_TO_PRICE(d) ((LuxPrice)((d) * LUX_PRICE_MULTIPLIER))
#define LUX_FROM_PRICE(p) ((double)(p) / LUX_PRICE_MULTIPLIER)
#define LUX_TO_QTY(d) ((LuxQuantity)((d) * LUX_PRICE_MULTIPLIER))
#define LUX_FROM_QTY(q) ((double)(q) / LUX_PRICE_MULTIPLIER)

// Order structure
typedef struct {
    uint64_t id;
    uint64_t symbol_id;
    uint64_t account_id;
    LuxPrice price;
    LuxQuantity quantity;
    LuxQuantity filled;
    LuxSide side;
    LuxOrderType order_type;
    LuxTimeInForce tif;
    LuxOrderStatus status;
    uint64_t stp_group;
    LuxPrice stop_price;
    int64_t timestamp_ns;
} LuxOrder;

// Trade structure
typedef struct {
    uint64_t id;
    uint64_t symbol_id;
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    uint64_t buyer_account_id;
    uint64_t seller_account_id;
    LuxPrice price;
    LuxQuantity quantity;
    LuxSide aggressor_side;
    int64_t timestamp_ns;
} LuxTrade;

// Depth level
typedef struct {
    double price;
    double quantity;
    int order_count;
} LuxDepthLevel;

// Market depth
typedef struct {
    LuxDepthLevel* bids;
    size_t bid_count;
    LuxDepthLevel* asks;
    size_t ask_count;
    int64_t timestamp_ns;
} LuxMarketDepth;

// Order result
typedef struct {
    bool success;
    uint64_t order_id;
    char error[256];
    LuxTrade* trades;
    size_t trade_count;
} LuxOrderResult;

// Cancel result
typedef struct {
    bool success;
    bool has_order;
    LuxOrder cancelled_order;
    char error[256];
} LuxCancelResult;

// Engine statistics
typedef struct {
    uint64_t total_orders_placed;
    uint64_t total_orders_cancelled;
    uint64_t total_trades;
    uint64_t total_volume;
} LuxEngineStats;

// Engine configuration
typedef struct {
    size_t worker_threads;
    size_t max_batch_size;
    bool enable_stp;
    bool async_mode;
} LuxEngineConfig;

// =============================================================================
// Engine API
// =============================================================================

// Create engine with default config
LuxEngine lux_engine_create(void);

// Create engine with config
LuxEngine lux_engine_create_with_config(const LuxEngineConfig* config);

// Destroy engine
void lux_engine_destroy(LuxEngine engine);

// Start engine
void lux_engine_start(LuxEngine engine);

// Stop engine
void lux_engine_stop(LuxEngine engine);

// Check if running
bool lux_engine_is_running(LuxEngine engine);

// Add symbol
bool lux_engine_add_symbol(LuxEngine engine, uint64_t symbol_id);

// Remove symbol
bool lux_engine_remove_symbol(LuxEngine engine, uint64_t symbol_id);

// Check if symbol exists
bool lux_engine_has_symbol(LuxEngine engine, uint64_t symbol_id);

// Get symbols (caller must free result)
uint64_t* lux_engine_symbols(LuxEngine engine, size_t* count);

// Place order
LuxOrderResult lux_engine_place_order(LuxEngine engine, const LuxOrder* order);

// Cancel order
LuxCancelResult lux_engine_cancel_order(LuxEngine engine, uint64_t symbol_id, uint64_t order_id);

// Get order
bool lux_engine_get_order(LuxEngine engine, uint64_t symbol_id, uint64_t order_id, LuxOrder* out);

// Get market depth
LuxMarketDepth lux_engine_get_depth(LuxEngine engine, uint64_t symbol_id, size_t levels);

// Get best bid (returns false if no bids)
bool lux_engine_best_bid(LuxEngine engine, uint64_t symbol_id, LuxPrice* price);

// Get best ask (returns false if no asks)
bool lux_engine_best_ask(LuxEngine engine, uint64_t symbol_id, LuxPrice* price);

// Get statistics
LuxEngineStats lux_engine_get_stats(LuxEngine engine);

// =============================================================================
// OrderBook API (direct access, use with caution)
// =============================================================================

// Get orderbook for symbol
LuxOrderBook lux_engine_get_orderbook(LuxEngine engine, uint64_t symbol_id);

// Place order on orderbook directly
LuxOrderResult lux_orderbook_place_order(LuxOrderBook book, const LuxOrder* order);

// Cancel order on orderbook
LuxCancelResult lux_orderbook_cancel_order(LuxOrderBook book, uint64_t order_id);

// Get order from orderbook
bool lux_orderbook_get_order(LuxOrderBook book, uint64_t order_id, LuxOrder* out);

// Get depth from orderbook
LuxMarketDepth lux_orderbook_get_depth(LuxOrderBook book, size_t levels);

// Get orderbook statistics
size_t lux_orderbook_bid_levels(LuxOrderBook book);
size_t lux_orderbook_ask_levels(LuxOrderBook book);
size_t lux_orderbook_total_orders(LuxOrderBook book);

// =============================================================================
// Memory management
// =============================================================================

// Free order result (trades array)
void lux_order_result_free(LuxOrderResult* result);

// Free market depth
void lux_market_depth_free(LuxMarketDepth* depth);

// Free symbol array
void lux_symbols_free(uint64_t* symbols);

// =============================================================================
// Utility
// =============================================================================

// Generate unique order ID
uint64_t lux_generate_order_id(void);

// Reset order ID generator
void lux_reset_order_id_generator(uint64_t start);

#ifdef __cplusplus
}
#endif

#endif // LUXDEX_C_H
