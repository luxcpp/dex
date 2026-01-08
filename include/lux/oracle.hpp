#ifndef LUX_ORACLE_HPP
#define LUX_ORACLE_HPP

#include <map>
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include <vector>
#include <algorithm>
#include <cmath>

#include "types.hpp"

namespace lux {

// =============================================================================
// Price Data from Single Source
// =============================================================================

struct SourcePriceData {
    PriceSource source;
    I128 price_x18;
    I128 confidence_x18;    // Price confidence interval
    uint64_t timestamp;
    uint64_t block_number;
    bool is_valid;
};

// =============================================================================
// Aggregated Price Data
// =============================================================================

struct AggregatedPriceData {
    I128 price_x18;
    I128 confidence_x18;
    I128 deviation_x18;          // Standard deviation across sources
    uint8_t num_sources;
    uint64_t timestamp;
    AggregationMethod method;
};

// =============================================================================
// Oracle Configuration
// =============================================================================

struct OracleConfig {
    uint64_t asset_id;           // Unique asset identifier
    Currency base_token;
    Currency quote_token;
    uint64_t max_staleness;      // Maximum age in seconds
    I128 max_deviation_x18;      // Maximum deviation between sources
    AggregationMethod method;
    std::vector<PriceSource> sources;
    std::vector<I128> weights_x18;  // Source weights for weighted methods
};

// =============================================================================
// Robust Index Parameters
// =============================================================================

struct RobustParams {
    uint8_t min_sources;         // Minimum sources required
    I128 outlier_threshold_x18;  // Z-score threshold for outlier detection
    I128 trim_percent_x18;       // Percentage to trim for trimmed mean
    bool use_volume_weighting;   // Weight by source volume
};

// =============================================================================
// LXOracle - Multi-Source Price Aggregation
// =============================================================================

class LXOracle {
public:
    LXOracle();
    ~LXOracle() = default;

    // Non-copyable
    LXOracle(const LXOracle&) = delete;
    LXOracle& operator=(const LXOracle&) = delete;

    // =========================================================================
    // Configuration
    // =========================================================================

    int32_t register_asset(const OracleConfig& config);
    int32_t update_config(uint64_t asset_id, const OracleConfig& config);
    std::optional<OracleConfig> get_config(uint64_t asset_id) const;

    void set_robust_params(uint64_t asset_id, const RobustParams& params);
    std::optional<RobustParams> get_robust_params(uint64_t asset_id) const;

    // =========================================================================
    // Price Updates (from external sources)
    // =========================================================================

    // Update price from a specific source
    int32_t update_price(uint64_t asset_id, PriceSource source,
                         I128 price_x18, I128 confidence_x18,
                         uint64_t timestamp = 0);

    // Batch update multiple prices
    int32_t update_prices(const std::vector<std::tuple<uint64_t, PriceSource, I128, I128>>& updates);

    // =========================================================================
    // Price Queries
    // =========================================================================

    // Get latest aggregated price
    std::optional<I128> get_price(uint64_t asset_id) const;

    // Get price with full data
    std::optional<AggregatedPriceData> get_price_data(uint64_t asset_id) const;

    // Get prices for multiple assets
    std::vector<std::pair<uint64_t, I128>> get_prices(const std::vector<uint64_t>& asset_ids) const;

    // Get price from specific source
    std::optional<SourcePriceData> get_source_price(uint64_t asset_id, PriceSource source) const;

    // Get all source prices for an asset
    std::vector<SourcePriceData> get_all_source_prices(uint64_t asset_id) const;

    // =========================================================================
    // Index Price (Robust Construction)
    // =========================================================================

    // Get index price with outlier filtering
    std::optional<I128> index_price(uint64_t asset_id) const;

    // Get detailed index construction
    struct IndexPriceDetail {
        I128 price_x18;
        I128 median_x18;
        I128 mean_x18;
        I128 std_dev_x18;
        uint8_t sources_used;
        uint8_t outliers_filtered;
        std::vector<PriceSource> filtered_sources;
    };
    std::optional<IndexPriceDetail> index_price_detailed(uint64_t asset_id) const;

    // =========================================================================
    // TWAP Interface
    // =========================================================================

    // Get TWAP over window
    std::optional<I128> get_twap(uint64_t asset_id, uint64_t window_seconds) const;

    // Record price for TWAP calculation
    void record_twap_price(uint64_t asset_id, I128 price_x18, uint64_t timestamp);

    // =========================================================================
    // Staleness & Validity
    // =========================================================================

    bool is_price_fresh(uint64_t asset_id) const;
    bool is_price_fresh(uint64_t asset_id, uint64_t max_staleness) const;
    uint64_t price_age(uint64_t asset_id) const;

    // =========================================================================
    // Statistics
    // =========================================================================

    struct Stats {
        uint64_t total_assets;
        uint64_t total_updates;
        uint64_t stale_prices;
    };
    Stats get_stats() const;

private:
    // Asset configurations
    std::unordered_map<uint64_t, OracleConfig> configs_;
    std::unordered_map<uint64_t, RobustParams> robust_params_;
    mutable std::shared_mutex config_mutex_;

    // Price data: asset_id -> source -> price_data
    std::unordered_map<uint64_t, std::unordered_map<uint8_t, SourcePriceData>> prices_;
    mutable std::shared_mutex prices_mutex_;

    // TWAP data: asset_id -> [(timestamp, price)]
    std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, I128>>> twap_data_;
    mutable std::shared_mutex twap_mutex_;

    // Statistics
    std::atomic<uint64_t> total_updates_{0};

    // Aggregation methods
    I128 aggregate_median(const std::vector<I128>& prices) const;
    I128 aggregate_mean(const std::vector<I128>& prices) const;
    I128 aggregate_trimmed_mean(const std::vector<I128>& prices, I128 trim_percent_x18) const;
    I128 aggregate_weighted_median(const std::vector<I128>& prices,
                                    const std::vector<I128>& weights) const;

    // Outlier detection
    std::vector<bool> detect_outliers(const std::vector<I128>& prices, I128 threshold_x18) const;

    // Helper
    uint64_t current_timestamp() const;
};

// =============================================================================
// Oracle Source Adapter Interface
// =============================================================================

class IOracleSource {
public:
    virtual ~IOracleSource() = default;

    virtual PriceSource source_type() const = 0;
    virtual bool is_available() const = 0;

    // Fetch latest price for an asset
    virtual std::optional<SourcePriceData> fetch_price(uint64_t asset_id) = 0;

    // Fetch prices for multiple assets
    virtual std::vector<std::pair<uint64_t, SourcePriceData>>
    fetch_prices(const std::vector<uint64_t>& asset_ids) = 0;
};

// Chainlink adapter
class ChainlinkAdapter : public IOracleSource {
public:
    explicit ChainlinkAdapter(const Address& registry);

    PriceSource source_type() const override { return PriceSource::CHAINLINK; }
    bool is_available() const override;
    std::optional<SourcePriceData> fetch_price(uint64_t asset_id) override;
    std::vector<std::pair<uint64_t, SourcePriceData>>
    fetch_prices(const std::vector<uint64_t>& asset_ids) override;

private:
    Address registry_;
    std::unordered_map<uint64_t, Address> feed_addresses_;
};

// Pyth adapter
class PythAdapter : public IOracleSource {
public:
    explicit PythAdapter(const Address& pyth_contract);

    PriceSource source_type() const override { return PriceSource::PYTH; }
    bool is_available() const override;
    std::optional<SourcePriceData> fetch_price(uint64_t asset_id) override;
    std::vector<std::pair<uint64_t, SourcePriceData>>
    fetch_prices(const std::vector<uint64_t>& asset_ids) override;

private:
    Address pyth_contract_;
    std::unordered_map<uint64_t, std::array<uint8_t, 32>> price_ids_;
};

// LXPool adapter (on-chain AMM prices)
class LXPoolAdapter : public IOracleSource {
public:
    PriceSource source_type() const override { return PriceSource::LXPOOL; }
    bool is_available() const override { return true; }
    std::optional<SourcePriceData> fetch_price(uint64_t asset_id) override;
    std::vector<std::pair<uint64_t, SourcePriceData>>
    fetch_prices(const std::vector<uint64_t>& asset_ids) override;

private:
    // Reference to LXPool for price queries
};

} // namespace lux

#endif // LUX_ORACLE_HPP
