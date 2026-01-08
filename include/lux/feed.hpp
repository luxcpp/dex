#ifndef LUX_FEED_HPP
#define LUX_FEED_HPP

#include <map>
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include <vector>
#include <cmath>

#include "types.hpp"
#include "oracle.hpp"

namespace lux {

// =============================================================================
// Price Type Enum
// =============================================================================

enum class PriceType : uint8_t {
    INDEX = 0,      // Spot index from oracle
    MARK = 1,       // Mark price (index + premium)
    LAST = 2,       // Last trade price
    MID = 3,        // Mid price (best bid + best ask) / 2
    ORACLE = 4      // Raw oracle price
};

// =============================================================================
// Mark Price Configuration
// =============================================================================

struct MarkPriceConfig {
    uint64_t premium_ewma_window;    // EWMA window for premium (seconds)
    I128 impact_notional_x18;        // Notional for impact price calculation
    I128 max_premium_x18;            // Maximum premium cap
    I128 min_premium_x18;            // Minimum premium floor
    bool use_mid_price;              // Use mid price for premium calc
    bool cap_to_oracle;              // Cap mark to oracle bounds
};

// =============================================================================
// Funding Parameters
// =============================================================================

struct FundingParams {
    uint64_t funding_interval;       // Funding interval (typically 8h = 28800s)
    I128 max_funding_rate_x18;       // Max absolute funding rate per interval
    I128 interest_rate_x18;          // Base interest rate component
    I128 premium_fraction_x18;       // Fraction of premium used for funding
    bool use_twap_premium;           // Use TWAP of premium for funding
};

// =============================================================================
// Trigger Rule Types
// =============================================================================

enum class TriggerType : uint8_t {
    STOP_LOSS = 0,
    TAKE_PROFIT = 1,
    LIQUIDATION = 2,
    FUNDING = 3,
    ADL = 4
};

struct TriggerRule {
    TriggerType type;
    PriceType price_type;            // Which price to use for trigger
    bool use_mark_for_liquidation;   // Mark price for liquidation
    bool use_last_for_triggers;      // Last price for SL/TP
    I128 buffer_x18;                 // Price buffer for triggers
};

// =============================================================================
// LXFeed - Computed Price Feeds
// =============================================================================

class LXFeed {
public:
    explicit LXFeed(LXOracle& oracle);
    ~LXFeed() = default;

    // Non-copyable
    LXFeed(const LXFeed&) = delete;
    LXFeed& operator=(const LXFeed&) = delete;

    // =========================================================================
    // Configuration
    // =========================================================================

    void set_mark_price_config(uint32_t market_id, const MarkPriceConfig& config);
    std::optional<MarkPriceConfig> get_mark_price_config(uint32_t market_id) const;

    void set_funding_params(uint32_t market_id, const FundingParams& params);
    std::optional<FundingParams> get_funding_params(uint32_t market_id) const;

    void set_trigger_rules(uint32_t market_id, const std::vector<TriggerRule>& rules);

    // =========================================================================
    // Index Price
    // =========================================================================

    // Get index price from oracle
    std::optional<I128> index_price(uint32_t market_id) const;

    // Get index price with timestamp
    std::optional<std::pair<I128, uint64_t>> index_price_with_time(uint32_t market_id) const;

    // =========================================================================
    // Mark Price
    // =========================================================================

    // Mark = Index + EWMA(Premium)
    std::optional<I128> mark_price(uint32_t market_id) const;

    // Full mark price data
    std::optional<LXMarkPrice> get_mark_price(uint32_t market_id) const;

    // =========================================================================
    // Last Trade Price
    // =========================================================================

    std::optional<I128> last_price(uint32_t market_id) const;
    void update_last_price(uint32_t market_id, I128 price_x18, uint64_t timestamp = 0);

    // =========================================================================
    // Mid Price
    // =========================================================================

    std::optional<I128> mid_price(uint32_t market_id) const;
    void update_bbo(uint32_t market_id, I128 best_bid_x18, I128 best_ask_x18);

    // =========================================================================
    // Generic Price Query
    // =========================================================================

    std::optional<I128> get_price(uint32_t market_id, PriceType type) const;

    // Get all prices for a market
    struct AllPrices {
        I128 index_x18;
        I128 mark_x18;
        I128 last_x18;
        I128 mid_x18;
        uint64_t timestamp;
    };
    std::optional<AllPrices> get_all_prices(uint32_t market_id) const;

    // Get prices for multiple markets
    std::vector<std::pair<uint32_t, AllPrices>>
    get_multiple_market_prices(const std::vector<uint32_t>& market_ids) const;

    // =========================================================================
    // Premium & Basis
    // =========================================================================

    // Premium = Mark - Index
    std::optional<I128> premium(uint32_t market_id) const;

    // Basis = (Mark - Index) / Index (percentage)
    std::optional<I128> basis(uint32_t market_id) const;

    // Premium EWMA (exponentially weighted moving average)
    std::optional<I128> premium_ewma(uint32_t market_id) const;

    // Update premium for EWMA calculation
    void record_premium(uint32_t market_id, I128 premium_x18, uint64_t timestamp = 0);

    // =========================================================================
    // Funding Rate
    // =========================================================================

    // Get current funding rate
    std::optional<I128> funding_rate(uint32_t market_id) const;

    // Get funding rate with next funding time
    std::optional<LXFundingRate> get_funding_rate(uint32_t market_id) const;

    // Get funding interval
    uint64_t funding_interval(uint32_t market_id) const;

    // Get max funding rate
    I128 max_funding_rate(uint32_t market_id) const;

    // Predicted funding rate at next interval
    std::optional<I128> predicted_funding_rate(uint32_t market_id) const;

    // Calculate and update funding rate
    void calculate_funding_rate(uint32_t market_id);

    // =========================================================================
    // Trigger Price (for SL/TP, Liquidation)
    // =========================================================================

    // Get price for trigger evaluation
    std::optional<I128> get_trigger_price(uint32_t market_id, bool is_buy) const;

    // Check if a trigger condition is met
    bool check_trigger(uint32_t market_id, TriggerType type,
                       bool is_buy, I128 trigger_price_x18) const;

    // Liquidation price for display
    std::optional<I128> liquidation_price(uint32_t market_id, const LXPosition& position,
                                           I128 maintenance_margin_x18) const;

    // =========================================================================
    // Market Registration
    // =========================================================================

    int32_t register_market(uint32_t market_id, uint64_t asset_id);
    void unregister_market(uint32_t market_id);
    bool market_exists(uint32_t market_id) const;

    // =========================================================================
    // Statistics
    // =========================================================================

    struct Stats {
        uint64_t total_markets;
        uint64_t total_price_updates;
        uint64_t funding_calculations;
    };
    Stats get_stats() const;

private:
    LXOracle& oracle_;

    // Market -> asset mapping
    std::unordered_map<uint32_t, uint64_t> market_assets_;
    mutable std::shared_mutex market_mutex_;

    // Configurations
    std::unordered_map<uint32_t, MarkPriceConfig> mark_configs_;
    std::unordered_map<uint32_t, FundingParams> funding_params_;
    std::unordered_map<uint32_t, std::vector<TriggerRule>> trigger_rules_;
    mutable std::shared_mutex config_mutex_;

    // Price state
    struct MarketPriceState {
        I128 last_price_x18;
        I128 best_bid_x18;
        I128 best_ask_x18;
        I128 premium_ewma_x18;
        I128 current_funding_rate_x18;
        uint64_t last_price_time;
        uint64_t last_funding_calc_time;
        uint64_t next_funding_time;

        // Premium history for EWMA
        std::vector<std::pair<uint64_t, I128>> premium_history;
    };
    std::unordered_map<uint32_t, MarketPriceState> price_states_;
    mutable std::shared_mutex price_mutex_;

    // Statistics
    std::atomic<uint64_t> total_price_updates_{0};
    std::atomic<uint64_t> funding_calculations_{0};

    // Internal helpers
    MarketPriceState* get_price_state(uint32_t market_id);
    const MarketPriceState* get_price_state(uint32_t market_id) const;

    // EWMA calculation
    I128 calculate_ewma(const std::vector<std::pair<uint64_t, I128>>& history,
                        uint64_t window_seconds, uint64_t current_time) const;

    // Funding rate calculation
    // funding_rate = clamp(premium_twap * fraction + interest_rate, -max, +max)
    I128 compute_funding_rate(const MarketPriceState& state,
                               const FundingParams& params) const;

    // Helper
    uint64_t current_timestamp() const;
};

// =============================================================================
// Trigger Price Rules (from ILXFeed.sol documentation)
// =============================================================================
//
// Stop Loss / Take Profit:
//   - Uses LAST (trade) price by default
//   - Trigger when: (isBuy && lastPrice <= triggerPrice) || (!isBuy && lastPrice >= triggerPrice)
//
// Liquidation:
//   - Uses MARK price
//   - Account liquidatable when marginRatio < maintenanceMargin
//
// Funding:
//   - Calculated at funding intervals (default 8h)
//   - Uses TWAP of premium over funding interval
//   - Long pays short if funding positive, vice versa
//
// ADL (Auto-Deleverage):
//   - Triggered when insurance fund exhausted during liquidation
//   - Uses MARK price for valuation
//
// =============================================================================

} // namespace lux

#endif // LUX_FEED_HPP
