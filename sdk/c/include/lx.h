/*
 * LX C SDK
 * Copyright (c) 2025 Lux Partners Limited
 *
 * High-performance C client for LX trading.
 */

#ifndef LX_H
#define LX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version */
#define LX_VERSION_MAJOR 1
#define LX_VERSION_MINOR 0
#define LX_VERSION_PATCH 0

/* Error codes */
typedef enum {
    LX_OK = 0,
    LX_ERR_INVALID_ARG = -1,
    LX_ERR_NO_MEMORY = -2,
    LX_ERR_CONNECTION = -3,
    LX_ERR_TIMEOUT = -4,
    LX_ERR_AUTH = -5,
    LX_ERR_PARSE = -6,
    LX_ERR_PROTOCOL = -7,
    LX_ERR_RATE_LIMIT = -8,
    LX_ERR_ORDER_REJECTED = -9,
    LX_ERR_NOT_CONNECTED = -10,
    LX_ERR_INTERNAL = -99
} lx_error_t;

/* Order types */
typedef enum {
    LX_ORDER_LIMIT = 0,
    LX_ORDER_MARKET = 1,
    LX_ORDER_STOP = 2,
    LX_ORDER_STOP_LIMIT = 3,
    LX_ORDER_ICEBERG = 4,
    LX_ORDER_PEG = 5
} lx_order_type_t;

/* Order sides */
typedef enum {
    LX_SIDE_BUY = 0,
    LX_SIDE_SELL = 1
} lx_side_t;

/* Order status */
typedef enum {
    LX_STATUS_OPEN = 0,
    LX_STATUS_PARTIAL = 1,
    LX_STATUS_FILLED = 2,
    LX_STATUS_CANCELLED = 3,
    LX_STATUS_REJECTED = 4
} lx_order_status_t;

/* Time in force */
typedef enum {
    LX_TIF_GTC = 0,  /* Good Till Cancelled */
    LX_TIF_IOC = 1,  /* Immediate Or Cancel */
    LX_TIF_FOK = 2,  /* Fill Or Kill */
    LX_TIF_DAY = 3   /* Day Order */
} lx_time_in_force_t;

/* Connection state */
typedef enum {
    LX_STATE_DISCONNECTED = 0,
    LX_STATE_CONNECTING = 1,
    LX_STATE_CONNECTED = 2,
    LX_STATE_AUTHENTICATED = 3,
    LX_STATE_ERROR = 4
} lx_conn_state_t;

/* Forward declarations */
typedef struct lx_client lx_client_t;
typedef struct lx_order lx_order_t;
typedef struct lx_trade lx_trade_t;
typedef struct lx_orderbook lx_orderbook_t;
typedef struct lx_price_level lx_price_level_t;
typedef struct lx_balance lx_balance_t;
typedef struct lx_position lx_position_t;

/* Strings - fixed size for predictable memory */
#define LX_SYMBOL_LEN 32
#define LX_USER_ID_LEN 64
#define LX_CLIENT_ID_LEN 64
#define LX_MSG_LEN 256

/* Order structure */
struct lx_order {
    uint64_t order_id;
    char symbol[LX_SYMBOL_LEN];
    lx_order_type_t type;
    lx_side_t side;
    double price;
    double size;
    double filled;
    double remaining;
    lx_order_status_t status;
    char user_id[LX_USER_ID_LEN];
    char client_id[LX_CLIENT_ID_LEN];
    int64_t timestamp;
    lx_time_in_force_t time_in_force;
    bool post_only;
    bool reduce_only;
};

/* Trade structure */
struct lx_trade {
    uint64_t trade_id;
    char symbol[LX_SYMBOL_LEN];
    double price;
    double size;
    lx_side_t side;
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    char buyer_id[LX_USER_ID_LEN];
    char seller_id[LX_USER_ID_LEN];
    int64_t timestamp;
};

/* Price level in orderbook */
struct lx_price_level {
    double price;
    double size;
    int32_t count;
};

/* Order book structure */
struct lx_orderbook {
    char symbol[LX_SYMBOL_LEN];
    lx_price_level_t *bids;
    size_t bids_count;
    size_t bids_capacity;
    lx_price_level_t *asks;
    size_t asks_count;
    size_t asks_capacity;
    int64_t timestamp;
};

/* Balance structure */
struct lx_balance {
    char asset[LX_SYMBOL_LEN];
    double available;
    double locked;
    double total;
};

/* Position structure */
struct lx_position {
    char symbol[LX_SYMBOL_LEN];
    double size;
    double entry_price;
    double mark_price;
    double pnl;
    double margin;
};

/* Client configuration */
typedef struct {
    const char *ws_url;         /* WebSocket URL (default: ws://localhost:8081) */
    const char *api_key;        /* API key for authentication */
    const char *api_secret;     /* API secret for authentication */
    int connect_timeout_ms;     /* Connection timeout (default: 10000) */
    int recv_timeout_ms;        /* Receive timeout (default: 30000) */
    int reconnect_interval_ms;  /* Reconnect interval (default: 5000) */
    bool auto_reconnect;        /* Auto reconnect on disconnect */
} lx_config_t;

/* Callbacks */
typedef void (*lx_on_connect_t)(lx_client_t *client, void *user_data);
typedef void (*lx_on_disconnect_t)(lx_client_t *client, int code, const char *reason, void *user_data);
typedef void (*lx_on_error_t)(lx_client_t *client, lx_error_t error, const char *msg, void *user_data);
typedef void (*lx_on_order_update_t)(lx_client_t *client, const lx_order_t *order, void *user_data);
typedef void (*lx_on_trade_t)(lx_client_t *client, const lx_trade_t *trade, void *user_data);
typedef void (*lx_on_orderbook_t)(lx_client_t *client, const lx_orderbook_t *book, void *user_data);
typedef void (*lx_on_balance_t)(lx_client_t *client, const lx_balance_t *balance, void *user_data);
typedef void (*lx_on_position_t)(lx_client_t *client, const lx_position_t *position, void *user_data);

/* Callback structure */
typedef struct {
    lx_on_connect_t on_connect;
    lx_on_disconnect_t on_disconnect;
    lx_on_error_t on_error;
    lx_on_order_update_t on_order_update;
    lx_on_trade_t on_trade;
    lx_on_orderbook_t on_orderbook;
    lx_on_balance_t on_balance;
    lx_on_position_t on_position;
    void *user_data;
} lx_callbacks_t;

/*
 * Client lifecycle
 */

/* Create a new client with configuration */
lx_client_t *lx_client_new(const lx_config_t *config);

/* Set callbacks */
void lx_client_set_callbacks(lx_client_t *client, const lx_callbacks_t *callbacks);

/* Connect to the DEX */
lx_error_t lx_client_connect(lx_client_t *client);

/* Authenticate with API credentials */
lx_error_t lx_client_auth(lx_client_t *client);

/* Disconnect from the DEX */
void lx_client_disconnect(lx_client_t *client);

/* Free client resources */
void lx_client_free(lx_client_t *client);

/* Get connection state */
lx_conn_state_t lx_client_state(const lx_client_t *client);

/* Service the client (call in event loop) */
int lx_client_service(lx_client_t *client, int timeout_ms);

/*
 * Order operations
 */

/* Place a new order */
lx_error_t lx_place_order(
    lx_client_t *client,
    const lx_order_t *order,
    uint64_t *order_id_out
);

/* Cancel an order */
lx_error_t lx_cancel_order(
    lx_client_t *client,
    uint64_t order_id
);

/* Cancel all orders for a symbol */
lx_error_t lx_cancel_all_orders(
    lx_client_t *client,
    const char *symbol
);

/* Modify an existing order */
lx_error_t lx_modify_order(
    lx_client_t *client,
    uint64_t order_id,
    double new_price,
    double new_size
);

/*
 * Market data
 */

/* Subscribe to orderbook updates */
lx_error_t lx_subscribe_orderbook(
    lx_client_t *client,
    const char *symbol
);

/* Subscribe to trade updates */
lx_error_t lx_subscribe_trades(
    lx_client_t *client,
    const char *symbol
);

/* Unsubscribe from a channel */
lx_error_t lx_unsubscribe(
    lx_client_t *client,
    const char *channel
);

/* Get orderbook snapshot (blocking) */
lx_error_t lx_get_orderbook(
    lx_client_t *client,
    const char *symbol,
    int depth,
    lx_orderbook_t *book_out
);

/* Get recent trades (blocking) */
lx_error_t lx_get_trades(
    lx_client_t *client,
    const char *symbol,
    int limit,
    lx_trade_t **trades_out,
    size_t *count_out
);

/*
 * Account operations
 */

/* Get balances */
lx_error_t lx_get_balances(
    lx_client_t *client,
    lx_balance_t **balances_out,
    size_t *count_out
);

/* Get positions */
lx_error_t lx_get_positions(
    lx_client_t *client,
    lx_position_t **positions_out,
    size_t *count_out
);

/* Get open orders */
lx_error_t lx_get_orders(
    lx_client_t *client,
    const char *symbol,
    lx_order_t **orders_out,
    size_t *count_out
);

/*
 * Order book utilities
 */

/* Initialize an orderbook */
lx_error_t lx_orderbook_init(lx_orderbook_t *book, size_t initial_capacity);

/* Free orderbook resources */
void lx_orderbook_free(lx_orderbook_t *book);

/* Get best bid price */
double lx_orderbook_best_bid(const lx_orderbook_t *book);

/* Get best ask price */
double lx_orderbook_best_ask(const lx_orderbook_t *book);

/* Get spread */
double lx_orderbook_spread(const lx_orderbook_t *book);

/* Get mid price */
double lx_orderbook_mid(const lx_orderbook_t *book);

/*
 * Order utilities
 */

/* Initialize an order with defaults */
void lx_order_init(lx_order_t *order);

/* Create a limit order */
void lx_order_limit(
    lx_order_t *order,
    const char *symbol,
    lx_side_t side,
    double price,
    double size
);

/* Create a market order */
void lx_order_market(
    lx_order_t *order,
    const char *symbol,
    lx_side_t side,
    double size
);

/*
 * Memory management
 */

/* Free array allocated by SDK */
void lx_free(void *ptr);

/* Free trades array */
void lx_trades_free(lx_trade_t *trades, size_t count);

/* Free balances array */
void lx_balances_free(lx_balance_t *balances, size_t count);

/* Free positions array */
void lx_positions_free(lx_position_t *positions, size_t count);

/* Free orders array */
void lx_orders_free(lx_order_t *orders, size_t count);

/*
 * Error handling
 */

/* Get error string */
const char *lx_strerror(lx_error_t error);

/* Get last error message (thread-local) */
const char *lx_last_error(void);

/*
 * Utility functions
 */

/* Get library version string */
const char *lx_version(void);

/* Initialize library (call once at startup) */
lx_error_t lx_init(void);

/* Cleanup library (call once at shutdown) */
void lx_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* LX_H */
