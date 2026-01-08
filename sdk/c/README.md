# LX C SDK

High-performance C client for LX trading.

## Features

- WebSocket client using libwebsockets
- Async event-driven architecture
- Full orderbook management
- Order placement, modification, cancellation
- Market data subscriptions
- Thread-safe with minimal locking
- C11 with no external dependencies beyond libwebsockets

## Requirements

- CMake 3.10+
- C11 compiler (GCC, Clang)
- libwebsockets 4.0+
- pthread

### macOS

```bash
brew install libwebsockets cmake
```

### Linux (Debian/Ubuntu)

```bash
apt install libwebsockets-dev cmake build-essential
```

## Build

```bash
cd sdk/c
mkdir build && cd build
cmake ..
make
```

### Options

```bash
cmake -DLX_BUILD_EXAMPLES=ON ..   # Build examples (default: ON)
cmake -DLX_BUILD_TESTS=ON ..      # Build tests (default: OFF)
```

## Usage

### Basic Connection

```c
#include "lx.h"

int main(void) {
    // Initialize library
    lx_init();

    // Configure client
    lx_config_t config = {
        .ws_url = "ws://localhost:8081",
        .api_key = "your-api-key",
        .api_secret = "your-api-secret",
    };

    // Create client
    lx_client_t *client = lx_client_new(&config);
    if (!client) return 1;

    // Set callbacks
    lx_callbacks_t callbacks = {
        .on_connect = my_on_connect,
        .on_error = my_on_error,
        .on_orderbook = my_on_orderbook,
    };
    lx_client_set_callbacks(client, &callbacks);

    // Connect
    lx_client_connect(client);

    // Event loop
    while (running) {
        lx_client_service(client, 100);
    }

    // Cleanup
    lx_client_free(client);
    lx_cleanup();
    return 0;
}
```

### Placing Orders

```c
// Limit order
lx_order_t order;
lx_order_limit(&order, "BTC-USD", LX_SIDE_BUY, 50000.0, 0.1);
order.post_only = true;

uint64_t order_id;
lx_error_t err = lx_place_order(client, &order, &order_id);
if (err != LX_OK) {
    fprintf(stderr, "Order failed: %s\n", lx_strerror(err));
}

// Market order
lx_order_t market;
lx_order_market(&market, "BTC-USD", LX_SIDE_SELL, 0.05);
lx_place_order(client, &market, NULL);

// Cancel
lx_cancel_order(client, order_id);
```

### Market Data Subscriptions

```c
// Subscribe to orderbook
lx_subscribe_orderbook(client, "BTC-USD");

// Subscribe to trades
lx_subscribe_trades(client, "BTC-USD");

// Handle in callback
void on_orderbook(lx_client_t *client, const lx_orderbook_t *book, void *user_data) {
    printf("Best bid: %.2f, Best ask: %.2f\n",
        lx_orderbook_best_bid(book),
        lx_orderbook_best_ask(book));
}
```

### Local Orderbook Management

```c
// Initialize local orderbook
lx_orderbook_t book;
lx_orderbook_init(&book, 100);

// Update from feed
lx_orderbook_update_bid(&book, 50000.0, 1.5, 3);
lx_orderbook_update_ask(&book, 50001.0, 0.8, 2);

// Query
double spread = lx_orderbook_spread(&book);
double mid = lx_orderbook_mid(&book);

// Calculate market impact
double vwap = lx_orderbook_price_for_size(&book, LX_SIDE_BUY, 10.0);

// Cleanup
lx_orderbook_free(&book);
```

## API Reference

### Client Lifecycle

| Function | Description |
|----------|-------------|
| `lx_init()` | Initialize library (call once) |
| `lx_cleanup()` | Cleanup library (call once) |
| `lx_client_new(config)` | Create client |
| `lx_client_set_callbacks(client, cb)` | Set event callbacks |
| `lx_client_connect(client)` | Connect to DEX |
| `lx_client_auth(client)` | Authenticate |
| `lx_client_disconnect(client)` | Disconnect |
| `lx_client_free(client)` | Free client |
| `lx_client_state(client)` | Get connection state |
| `lx_client_service(client, timeout_ms)` | Process events |

### Orders

| Function | Description |
|----------|-------------|
| `lx_place_order(client, order, &id)` | Place order |
| `lx_cancel_order(client, id)` | Cancel order |
| `lx_modify_order(client, id, price, size)` | Modify order |
| `lx_order_init(order)` | Initialize order struct |
| `lx_order_limit(order, sym, side, price, size)` | Create limit order |
| `lx_order_market(order, sym, side, size)` | Create market order |

### Market Data

| Function | Description |
|----------|-------------|
| `lx_subscribe_orderbook(client, symbol)` | Subscribe to orderbook |
| `lx_subscribe_trades(client, symbol)` | Subscribe to trades |
| `lx_unsubscribe(client, channel)` | Unsubscribe |

### Orderbook Utilities

| Function | Description |
|----------|-------------|
| `lx_orderbook_init(book, capacity)` | Initialize orderbook |
| `lx_orderbook_free(book)` | Free orderbook |
| `lx_orderbook_best_bid(book)` | Get best bid |
| `lx_orderbook_best_ask(book)` | Get best ask |
| `lx_orderbook_spread(book)` | Get spread |
| `lx_orderbook_mid(book)` | Get mid price |

### Error Handling

| Function | Description |
|----------|-------------|
| `lx_strerror(err)` | Error code to string |
| `lx_last_error()` | Get last error message |

## Error Codes

| Code | Description |
|------|-------------|
| `LX_OK` | Success |
| `LX_ERR_INVALID_ARG` | Invalid argument |
| `LX_ERR_NO_MEMORY` | Out of memory |
| `LX_ERR_CONNECTION` | Connection error |
| `LX_ERR_TIMEOUT` | Operation timed out |
| `LX_ERR_AUTH` | Authentication failed |
| `LX_ERR_PARSE` | JSON parse error |
| `LX_ERR_PROTOCOL` | Protocol error |
| `LX_ERR_RATE_LIMIT` | Rate limit exceeded |
| `LX_ERR_ORDER_REJECTED` | Order rejected |
| `LX_ERR_NOT_CONNECTED` | Not connected |

## Connection States

| State | Description |
|-------|-------------|
| `LX_STATE_DISCONNECTED` | Not connected |
| `LX_STATE_CONNECTING` | Connection in progress |
| `LX_STATE_CONNECTED` | Connected, not authenticated |
| `LX_STATE_AUTHENTICATED` | Connected and authenticated |
| `LX_STATE_ERROR` | Error state |

## Thread Safety

- The client is thread-safe for sending messages from multiple threads
- Callbacks are invoked from the service thread
- Use external synchronization if callbacks access shared data

## Performance

- Event-driven, non-blocking I/O
- Lock-free message queue for sends
- Minimal allocations in hot path
- Binary search for orderbook operations

## Example

Run the basic example:

```bash
cd build
./basic -u ws://localhost:8081 -m BTC-USD
```

With authentication and test order:

```bash
./basic -k YOUR_API_KEY -s YOUR_API_SECRET -o
```

## License

Copyright (c) 2025 Lux Partners Limited. All rights reserved.
