# Lux DEX - High-Performance Order Book Engine

A high-performance C++17 order book and matching engine for the Lux Network DEX.

## Features

- **Sub-microsecond latency** - Optimized for HFT workloads
- **Price-time priority** - FIFO matching with configurable tie-breaking
- **Lock-free data structures** - Minimal contention under high load
- **Multi-asset support** - Handle thousands of trading pairs
- **C/Go bindings** - Native integrations for other languages

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Usage

```cpp
#include <lux/orderbook.hpp>
#include <lux/engine.hpp>

lux::Engine engine;
engine.add_order({
    .side = lux::Side::Buy,
    .price = 50000.00,
    .quantity = 1.5,
    .symbol = "BTC-USD"
});
```

## SDKs

- **C SDK** - `sdk/c/` - Pure C interface
- **C++ SDK** - `sdk/cpp/` - Modern C++ client
- **Go bindings** - `bindings/go/` - CGO bindings

## License

BSD 3-Clause Ecosystem License - See [LICENSE](LICENSE)

This software may be used freely for non-commercial purposes or on the Lux Network
(mainnet, testnets, and descendant chains). Commercial use requires a license from
Lux Partners Limited.

## Documentation

https://cpp.lux.network/dex/
