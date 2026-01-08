// Test for LXFeed implementation
// Compile: clang++ -std=c++17 -I ../include -o test_feed test_feed.cpp ../src/feed.cpp

#include "lux/feed.hpp"
#include <cassert>
#include <iostream>
#include <cmath>

using namespace lux;

// Mock oracle for testing
class MockOracle : public LXOracle {
public:
    void set_price(uint64_t asset_id, I128 price) {
        prices_[asset_id] = price;
    }

    std::optional<I128> index_price(uint64_t asset_id) const {
        auto it = prices_.find(asset_id);
        if (it == prices_.end()) return std::nullopt;
        return it->second;
    }

    std::optional<I128> get_price(uint64_t asset_id) const {
        return index_price(asset_id);
    }

    std::optional<AggregatedPriceData> get_price_data(uint64_t asset_id) const {
        auto it = prices_.find(asset_id);
        if (it == prices_.end()) return std::nullopt;
        AggregatedPriceData data;
        data.price_x18 = it->second;
        data.timestamp = 1704067200; // Fixed timestamp
        return data;
    }

private:
    std::unordered_map<uint64_t, I128> prices_;
};

// Test helpers
bool approx_equal(I128 a, I128 b, I128 tolerance = 1000000000000LL) { // 1e-6 tolerance
    I128 diff = a > b ? a - b : b - a;
    return diff <= tolerance;
}

void print_pass(const char* name) {
    std::cout << "[PASS] " << name << std::endl;
}

void print_fail(const char* name, const char* msg) {
    std::cout << "[FAIL] " << name << ": " << msg << std::endl;
}

// Tests
void test_market_registration() {
    LXOracle oracle;
    LXFeed feed(oracle);

    // Register market
    int32_t result = feed.register_market(1, 100);
    assert(result == errors::OK);
    assert(feed.market_exists(1));
    assert(!feed.market_exists(2));

    // Duplicate registration fails
    result = feed.register_market(1, 100);
    assert(result == errors::POOL_ALREADY_INITIALIZED);

    // Unregister
    feed.unregister_market(1);
    assert(!feed.market_exists(1));

    print_pass("test_market_registration");
}

void test_last_price() {
    LXOracle oracle;
    LXFeed feed(oracle);

    feed.register_market(1, 100);

    // No last price initially
    assert(!feed.last_price(1).has_value());

    // Update last price
    I128 price = x18::from_int(50000); // $50,000
    feed.update_last_price(1, price);

    auto result = feed.last_price(1);
    assert(result.has_value());
    assert(*result == price);

    print_pass("test_last_price");
}

void test_mid_price() {
    LXOracle oracle;
    LXFeed feed(oracle);

    feed.register_market(1, 100);

    // No mid price without BBO
    assert(!feed.mid_price(1).has_value());

    // Update BBO
    I128 bid = x18::from_int(49990);
    I128 ask = x18::from_int(50010);
    feed.update_bbo(1, bid, ask);

    auto result = feed.mid_price(1);
    assert(result.has_value());

    I128 expected_mid = x18::from_int(50000);
    assert(approx_equal(*result, expected_mid));

    print_pass("test_mid_price");
}

void test_mark_price_config() {
    LXOracle oracle;
    LXFeed feed(oracle);

    feed.register_market(1, 100);

    // No config initially
    assert(!feed.get_mark_price_config(1).has_value());

    // Set config
    MarkPriceConfig config;
    config.premium_ewma_window = 300;
    config.impact_notional_x18 = x18::from_int(100000);
    config.max_premium_x18 = x18::from_double(0.05); // 5%
    config.min_premium_x18 = x18::from_double(-0.05);
    config.use_mid_price = true;
    config.cap_to_oracle = true;

    feed.set_mark_price_config(1, config);

    auto result = feed.get_mark_price_config(1);
    assert(result.has_value());
    assert(result->premium_ewma_window == 300);

    print_pass("test_mark_price_config");
}

void test_funding_params() {
    LXOracle oracle;
    LXFeed feed(oracle);

    feed.register_market(1, 100);

    // Default funding interval
    assert(feed.funding_interval(1) == 28800); // 8 hours

    // Set params
    FundingParams params;
    params.funding_interval = 14400; // 4 hours
    params.max_funding_rate_x18 = x18::from_double(0.01); // 1%
    params.interest_rate_x18 = 0;
    params.premium_fraction_x18 = X18_ONE;
    params.use_twap_premium = true;

    feed.set_funding_params(1, params);

    assert(feed.funding_interval(1) == 14400);
    assert(feed.max_funding_rate(1) == params.max_funding_rate_x18);

    print_pass("test_funding_params");
}

void test_premium_and_basis() {
    LXOracle oracle;
    LXFeed feed(oracle);

    // Register asset in oracle and market in feed
    OracleConfig oconfig;
    oconfig.asset_id = 100;
    oconfig.max_staleness = 3600;
    oracle.register_asset(oconfig);

    I128 index = x18::from_int(50000);
    oracle.update_price(100, PriceSource::BINANCE, index, 0);

    feed.register_market(1, 100);

    // Record some premium
    I128 premium_val = x18::from_int(50); // $50 premium
    feed.record_premium(1, premium_val);

    // Check premium EWMA was updated
    auto ewma = feed.premium_ewma(1);
    assert(ewma.has_value());
    // EWMA with single point should be close to the value
    assert(approx_equal(*ewma, premium_val, x18::from_int(1)));

    print_pass("test_premium_and_basis");
}

void test_trigger_check() {
    LXOracle oracle;
    LXFeed feed(oracle);

    feed.register_market(1, 100);

    // Set last price
    I128 current_price = x18::from_int(50000);
    feed.update_last_price(1, current_price);

    // Buy trigger at 49000 should NOT be triggered (price is above trigger)
    bool triggered = feed.check_trigger(1, TriggerType::STOP_LOSS, true, x18::from_int(49000));
    assert(!triggered);

    // Buy trigger at 51000 SHOULD be triggered (price <= trigger)
    triggered = feed.check_trigger(1, TriggerType::STOP_LOSS, true, x18::from_int(51000));
    assert(triggered);

    // Sell trigger at 51000 should NOT be triggered (price is below trigger)
    triggered = feed.check_trigger(1, TriggerType::STOP_LOSS, false, x18::from_int(51000));
    assert(!triggered);

    // Sell trigger at 49000 SHOULD be triggered (price >= trigger)
    triggered = feed.check_trigger(1, TriggerType::STOP_LOSS, false, x18::from_int(49000));
    assert(triggered);

    print_pass("test_trigger_check");
}

void test_liquidation_price() {
    LXOracle oracle;
    LXFeed feed(oracle);

    // Setup oracle price
    OracleConfig oconfig;
    oconfig.asset_id = 100;
    oconfig.max_staleness = 3600;
    oracle.register_asset(oconfig);
    oracle.update_price(100, PriceSource::BINANCE, x18::from_int(50000), 0);

    feed.register_market(1, 100);

    // Long position
    LXPosition pos;
    pos.market_id = 1;
    pos.side = PositionSide::LONG;
    pos.size_x18 = x18::from_int(1); // 1 BTC
    pos.entry_px_x18 = x18::from_int(50000);

    I128 maintenance_margin = x18::from_int(500); // $500 maintenance

    auto liq = feed.liquidation_price(1, pos, maintenance_margin);
    assert(liq.has_value());

    // For long: liq_price = entry - margin/size = 50000 - 500 = 49500
    I128 expected = x18::from_int(49500);
    assert(approx_equal(*liq, expected));

    // Short position
    pos.side = PositionSide::SHORT;
    liq = feed.liquidation_price(1, pos, maintenance_margin);
    assert(liq.has_value());

    // For short: liq_price = entry + margin/size = 50000 + 500 = 50500
    expected = x18::from_int(50500);
    assert(approx_equal(*liq, expected));

    print_pass("test_liquidation_price");
}

void test_get_all_prices() {
    LXOracle oracle;
    LXFeed feed(oracle);

    // Setup
    OracleConfig oconfig;
    oconfig.asset_id = 100;
    oconfig.max_staleness = 3600;
    oracle.register_asset(oconfig);
    oracle.update_price(100, PriceSource::BINANCE, x18::from_int(50000), 0);

    feed.register_market(1, 100);
    feed.update_last_price(1, x18::from_int(50010));
    feed.update_bbo(1, x18::from_int(49995), x18::from_int(50005));

    auto prices = feed.get_all_prices(1);
    assert(prices.has_value());
    assert(approx_equal(prices->index_x18, x18::from_int(50000)));
    assert(approx_equal(prices->last_x18, x18::from_int(50010)));
    assert(approx_equal(prices->mid_x18, x18::from_int(50000)));

    print_pass("test_get_all_prices");
}

void test_stats() {
    LXOracle oracle;
    LXFeed feed(oracle);

    auto stats = feed.get_stats();
    assert(stats.total_markets == 0);
    assert(stats.total_price_updates == 0);

    feed.register_market(1, 100);
    feed.register_market(2, 200);

    stats = feed.get_stats();
    assert(stats.total_markets == 2);

    feed.update_last_price(1, x18::from_int(50000));
    feed.update_bbo(1, x18::from_int(49990), x18::from_int(50010));

    stats = feed.get_stats();
    assert(stats.total_price_updates == 2);

    print_pass("test_stats");
}

void test_funding_rate_calculation() {
    LXOracle oracle;
    LXFeed feed(oracle);

    // Setup
    OracleConfig oconfig;
    oconfig.asset_id = 100;
    oconfig.max_staleness = 3600;
    oracle.register_asset(oconfig);
    oracle.update_price(100, PriceSource::BINANCE, x18::from_int(50000), 0);

    feed.register_market(1, 100);

    // Set funding params
    FundingParams params;
    params.funding_interval = 28800;
    params.max_funding_rate_x18 = x18::from_double(0.01); // 1% max
    params.interest_rate_x18 = 0;
    params.premium_fraction_x18 = X18_ONE;
    params.use_twap_premium = true;
    feed.set_funding_params(1, params);

    // Record positive premium (futures trading above spot)
    I128 premium_val = x18::from_double(0.002); // 0.2% premium
    feed.record_premium(1, premium_val);

    // Calculate funding
    feed.calculate_funding_rate(1);

    auto rate = feed.get_funding_rate(1);
    assert(rate.has_value());
    // Funding rate should be positive (longs pay shorts)
    assert(rate->rate_x18 > 0);

    print_pass("test_funding_rate_calculation");
}

int main() {
    std::cout << "LXFeed Tests\n============\n";

    test_market_registration();
    test_last_price();
    test_mid_price();
    test_mark_price_config();
    test_funding_params();
    test_premium_and_basis();
    test_trigger_check();
    test_liquidation_price();
    test_get_all_prices();
    test_stats();
    test_funding_rate_calculation();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
