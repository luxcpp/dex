#include <iostream>
#include <cassert>
#include <vector>
#include <chrono>
#include <iomanip>

#include "lux/engine.hpp"

using namespace lux;

// Simple test framework
#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    test_##name(); \
    std::cout << "PASSED" << std::endl; \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "\nAssertion failed: " << #cond \
                  << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        std::abort(); \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "\nAssertion failed: " << #a << " == " << #b \
                  << " (" << (a) << " != " << (b) << ")" \
                  << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        std::abort(); \
    } \
} while(0)

// Test: Basic order placement
TEST(basic_order_placement) {
    OrderBook book(1);

    // Place a limit buy order
    Order buy = OrderBuilder()
        .id(1)
        .account(100)
        .side(Side::Buy)
        .type(OrderType::Limit)
        .price(100.0)
        .quantity(10.0)
        .tif(TimeInForce::GTC)
        .build();

    auto trades = book.place_order(buy);
    ASSERT(trades.empty());  // No matching orders, no trades
    ASSERT(book.has_order(1));
    ASSERT_EQ(book.total_orders(), 1u);
    ASSERT_EQ(book.bid_levels(), 1u);
    ASSERT_EQ(book.ask_levels(), 0u);

    auto retrieved = book.get_order(1);
    ASSERT(retrieved.has_value());
    ASSERT_EQ(retrieved->id, 1u);
    ASSERT_EQ(retrieved->price, Order::to_price(100.0));
}

// Test: Order matching
TEST(order_matching) {
    OrderBook book(1);

    // Place buy order
    Order buy = OrderBuilder()
        .id(1)
        .account(100)
        .side(Side::Buy)
        .type(OrderType::Limit)
        .price(100.0)
        .quantity(10.0)
        .tif(TimeInForce::GTC)
        .build();

    book.place_order(buy);

    // Place matching sell order
    Order sell = OrderBuilder()
        .id(2)
        .account(200)
        .side(Side::Sell)
        .type(OrderType::Limit)
        .price(100.0)
        .quantity(5.0)
        .tif(TimeInForce::GTC)
        .build();

    auto trades = book.place_order(sell);
    ASSERT_EQ(trades.size(), 1u);
    ASSERT_EQ(trades[0].buy_order_id, 1u);
    ASSERT_EQ(trades[0].sell_order_id, 2u);
    ASSERT_EQ(trades[0].quantity, Order::to_quantity(5.0));
    ASSERT_EQ(trades[0].price, Order::to_price(100.0));

    // Buy order should be partially filled
    auto remaining = book.get_order(1);
    ASSERT(remaining.has_value());
    ASSERT_EQ(remaining->filled, Order::to_quantity(5.0));
    ASSERT_EQ(remaining->remaining(), Order::to_quantity(5.0));

    // Sell order was fully filled and removed
    ASSERT(!book.has_order(2));
}

// Test: Price-time priority
TEST(price_time_priority) {
    OrderBook book(1);

    // Place multiple buy orders at different prices
    Order buy1 = OrderBuilder()
        .id(1).account(100).side(Side::Buy)
        .type(OrderType::Limit).price(99.0).quantity(10.0)
        .tif(TimeInForce::GTC).build();

    Order buy2 = OrderBuilder()
        .id(2).account(101).side(Side::Buy)
        .type(OrderType::Limit).price(100.0).quantity(10.0)
        .tif(TimeInForce::GTC).build();

    Order buy3 = OrderBuilder()
        .id(3).account(102).side(Side::Buy)
        .type(OrderType::Limit).price(100.0).quantity(10.0)
        .tif(TimeInForce::GTC).build();

    book.place_order(buy1);
    book.place_order(buy2);
    book.place_order(buy3);

    // Best bid should be 100.0
    auto best_bid = book.best_bid();
    ASSERT(best_bid.has_value());
    ASSERT_EQ(*best_bid, Order::to_price(100.0));

    // Sell should match order 2 first (best price, earliest time)
    Order sell = OrderBuilder()
        .id(4).account(200).side(Side::Sell)
        .type(OrderType::Limit).price(99.0).quantity(15.0)
        .tif(TimeInForce::GTC).build();

    auto trades = book.place_order(sell);
    ASSERT_EQ(trades.size(), 2u);
    ASSERT_EQ(trades[0].buy_order_id, 2u);  // Order 2 matched first (price 100)
    ASSERT_EQ(trades[1].buy_order_id, 3u);  // Order 3 matched second (same price, later time)
}

// Test: Self-trade prevention
TEST(self_trade_prevention) {
    OrderBook book(1);

    // Place buy order with STP group
    Order buy = OrderBuilder()
        .id(1).account(100).side(Side::Buy)
        .type(OrderType::Limit).price(100.0).quantity(10.0)
        .tif(TimeInForce::GTC).stp_group(999).build();

    book.place_order(buy);

    // Place sell order with same STP group
    Order sell = OrderBuilder()
        .id(2).account(100).side(Side::Sell)
        .type(OrderType::Limit).price(100.0).quantity(10.0)
        .tif(TimeInForce::GTC).stp_group(999).build();

    auto trades = book.place_order(sell);
    ASSERT(trades.empty());  // No trade due to STP

    // Buy order should have been cancelled
    ASSERT(!book.has_order(1));

    // Sell order should be in the book
    ASSERT(book.has_order(2));
}

// Test: IOC order
TEST(ioc_order) {
    OrderBook book(1);

    // Place buy order
    Order buy = OrderBuilder()
        .id(1).account(100).side(Side::Buy)
        .type(OrderType::Limit).price(100.0).quantity(5.0)
        .tif(TimeInForce::GTC).build();

    book.place_order(buy);

    // Place IOC sell for more than available
    Order sell = OrderBuilder()
        .id(2).account(200).side(Side::Sell)
        .type(OrderType::Limit).price(100.0).quantity(10.0)
        .tif(TimeInForce::IOC).build();

    auto trades = book.place_order(sell);
    ASSERT_EQ(trades.size(), 1u);
    ASSERT_EQ(trades[0].quantity, Order::to_quantity(5.0));

    // IOC order should not be in book
    ASSERT(!book.has_order(2));
    ASSERT_EQ(book.total_orders(), 0u);
}

// Test: FOK order
TEST(fok_order) {
    OrderBook book(1);

    // Place buy order for 5 units
    Order buy = OrderBuilder()
        .id(1).account(100).side(Side::Buy)
        .type(OrderType::Limit).price(100.0).quantity(5.0)
        .tif(TimeInForce::GTC).build();

    book.place_order(buy);

    // FOK sell for 10 - should be rejected (not enough liquidity)
    Order sell = OrderBuilder()
        .id(2).account(200).side(Side::Sell)
        .type(OrderType::Limit).price(100.0).quantity(10.0)
        .tif(TimeInForce::FOK).build();

    auto trades = book.place_order(sell);
    ASSERT(trades.empty());  // FOK rejected

    // Original buy should still be in book
    ASSERT(book.has_order(1));
}

// Test: Market order
TEST(market_order) {
    OrderBook book(1);

    // Place multiple ask orders
    Order ask1 = OrderBuilder()
        .id(1).account(100).side(Side::Sell)
        .type(OrderType::Limit).price(101.0).quantity(5.0)
        .tif(TimeInForce::GTC).build();

    Order ask2 = OrderBuilder()
        .id(2).account(101).side(Side::Sell)
        .type(OrderType::Limit).price(102.0).quantity(5.0)
        .tif(TimeInForce::GTC).build();

    book.place_order(ask1);
    book.place_order(ask2);

    // Market buy should take liquidity at any price
    Order buy = OrderBuilder()
        .id(3).account(200).side(Side::Buy)
        .type(OrderType::Market).quantity(7.0)
        .tif(TimeInForce::GTC).build();

    auto trades = book.place_order(buy);
    ASSERT_EQ(trades.size(), 2u);
    ASSERT_EQ(trades[0].price, Order::to_price(101.0));  // Best ask first
    ASSERT_EQ(trades[0].quantity, Order::to_quantity(5.0));
    ASSERT_EQ(trades[1].price, Order::to_price(102.0));  // Next level
    ASSERT_EQ(trades[1].quantity, Order::to_quantity(2.0));
}

// Test: Order cancellation
TEST(order_cancellation) {
    OrderBook book(1);

    Order buy = OrderBuilder()
        .id(1).account(100).side(Side::Buy)
        .type(OrderType::Limit).price(100.0).quantity(10.0)
        .tif(TimeInForce::GTC).build();

    book.place_order(buy);
    ASSERT(book.has_order(1));

    auto cancelled = book.cancel_order(1);
    ASSERT(cancelled.has_value());
    ASSERT_EQ(cancelled->id, 1u);
    ASSERT(cancelled->status == OrderStatus::Cancelled);
    ASSERT(!book.has_order(1));
    ASSERT_EQ(book.total_orders(), 0u);
}

// Test: Order modification
TEST(order_modification) {
    OrderBook book(1);

    Order buy = OrderBuilder()
        .id(1).account(100).side(Side::Buy)
        .type(OrderType::Limit).price(100.0).quantity(10.0)
        .tif(TimeInForce::GTC).build();

    book.place_order(buy);

    // Modify price and quantity
    auto modified = book.modify_order(1, Order::to_price(99.0), Order::to_quantity(20.0));
    ASSERT(modified.has_value());
    ASSERT_EQ(modified->price, Order::to_price(99.0));
    ASSERT_EQ(modified->quantity, Order::to_quantity(20.0));

    auto retrieved = book.get_order(1);
    ASSERT(retrieved.has_value());
    ASSERT_EQ(retrieved->price, Order::to_price(99.0));
}

// Test: Market depth
TEST(market_depth) {
    OrderBook book(1);

    // Add bids at different prices
    for (int i = 0; i < 5; ++i) {
        Order buy = OrderBuilder()
            .id(i + 1).account(100).side(Side::Buy)
            .type(OrderType::Limit).price(100.0 - i).quantity(10.0)
            .tif(TimeInForce::GTC).build();
        book.place_order(buy);
    }

    // Add asks at different prices
    for (int i = 0; i < 5; ++i) {
        Order sell = OrderBuilder()
            .id(i + 10).account(200).side(Side::Sell)
            .type(OrderType::Limit).price(101.0 + i).quantity(10.0)
            .tif(TimeInForce::GTC).build();
        book.place_order(sell);
    }

    auto depth = book.get_depth(3);
    ASSERT_EQ(depth.bids.size(), 3u);
    ASSERT_EQ(depth.asks.size(), 3u);

    // Best bid should be 100.0
    ASSERT_EQ(depth.bids[0].price, 100.0);
    // Best ask should be 101.0
    ASSERT_EQ(depth.asks[0].price, 101.0);
}

// Test: Engine multi-symbol
TEST(engine_multi_symbol) {
    Engine engine;
    engine.add_symbol(1);
    engine.add_symbol(2);

    ASSERT(engine.has_symbol(1));
    ASSERT(engine.has_symbol(2));
    ASSERT(!engine.has_symbol(3));

    auto symbols = engine.symbols();
    ASSERT_EQ(symbols.size(), 2u);

    // Place orders on different symbols
    Order buy1 = OrderBuilder()
        .id(1).symbol(1).account(100).side(Side::Buy)
        .type(OrderType::Limit).price(100.0).quantity(10.0)
        .tif(TimeInForce::GTC).build();

    Order buy2 = OrderBuilder()
        .id(2).symbol(2).account(100).side(Side::Buy)
        .type(OrderType::Limit).price(200.0).quantity(10.0)
        .tif(TimeInForce::GTC).build();

    auto result1 = engine.place_order(buy1);
    auto result2 = engine.place_order(buy2);

    ASSERT(result1.success);
    ASSERT(result2.success);

    // Verify orders are on correct symbols
    auto order1 = engine.get_order(1, 1);
    auto order2 = engine.get_order(2, 2);

    ASSERT(order1.has_value());
    ASSERT(order2.has_value());
    ASSERT_EQ(order1->symbol_id, 1u);
    ASSERT_EQ(order2->symbol_id, 2u);
}

// Test: Engine statistics
TEST(engine_statistics) {
    Engine engine;
    engine.add_symbol(1);

    // Place some orders
    for (int i = 0; i < 5; ++i) {
        Order buy = OrderBuilder()
            .id(i + 1).symbol(1).account(100).side(Side::Buy)
            .type(OrderType::Limit).price(100.0).quantity(10.0)
            .tif(TimeInForce::GTC).build();
        engine.place_order(buy);
    }

    // Place matching sell
    Order sell = OrderBuilder()
        .id(10).symbol(1).account(200).side(Side::Sell)
        .type(OrderType::Limit).price(100.0).quantity(25.0)
        .tif(TimeInForce::GTC).build();
    engine.place_order(sell);

    // Cancel remaining order
    engine.cancel_order(1, 5);

    auto stats = engine.get_stats();
    ASSERT_EQ(stats.total_orders_placed, 6u);
    ASSERT_EQ(stats.total_orders_cancelled, 1u);
    ASSERT(stats.total_trades > 0);
}

// Performance test
void bench_order_throughput() {
    std::cout << "\nRunning performance benchmark...\n";

    OrderBook book(1);
    const int NUM_ORDERS = 100000;

    auto start = std::chrono::high_resolution_clock::now();

    // Place orders
    for (int i = 0; i < NUM_ORDERS; ++i) {
        Order order = OrderBuilder()
            .id(i + 1)
            .account(i % 100)
            .side(i % 2 == 0 ? Side::Buy : Side::Sell)
            .type(OrderType::Limit)
            .price(100.0 + (i % 100) * 0.01)
            .quantity(1.0)
            .tif(TimeInForce::GTC)
            .build();

        book.place_order(order);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double orders_per_sec = (double)NUM_ORDERS / (duration.count() / 1e6);
    std::cout << "  Placed " << NUM_ORDERS << " orders in "
              << duration.count() / 1000.0 << " ms\n";
    std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
              << orders_per_sec << " orders/sec\n";
    std::cout << "  Latency: " << std::fixed << std::setprecision(2)
              << duration.count() / (double)NUM_ORDERS << " us/order\n";
}

int main() {
    std::cout << "=== LuxDEX Matching Engine Tests ===" << std::endl;

    RUN_TEST(basic_order_placement);
    RUN_TEST(order_matching);
    RUN_TEST(price_time_priority);
    RUN_TEST(self_trade_prevention);
    RUN_TEST(ioc_order);
    RUN_TEST(fok_order);
    RUN_TEST(market_order);
    RUN_TEST(order_cancellation);
    RUN_TEST(order_modification);
    RUN_TEST(market_depth);
    RUN_TEST(engine_multi_symbol);
    RUN_TEST(engine_statistics);

    std::cout << "\n=== All tests passed ===" << std::endl;

    bench_order_throughput();

    return 0;
}
