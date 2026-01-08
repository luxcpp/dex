// =============================================================================
// oracle.cpp - LXOracle Multi-Source Price Aggregation
// =============================================================================

#include "lux/oracle.hpp"
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace lux {

// =============================================================================
// Constructor
// =============================================================================

LXOracle::LXOracle() = default;

// =============================================================================
// Configuration
// =============================================================================

int32_t LXOracle::register_asset(const OracleConfig& config) {
    std::unique_lock lock(config_mutex_);

    if (configs_.find(config.asset_id) != configs_.end()) {
        return errors::POOL_ALREADY_INITIALIZED;
    }

    configs_[config.asset_id] = config;

    // Initialize with default robust params
    RobustParams params;
    params.min_sources = 1;
    params.outlier_threshold_x18 = x18::from_double(3.0); // 3 sigma
    params.trim_percent_x18 = x18::from_double(0.1);       // 10% trim
    params.use_volume_weighting = false;
    robust_params_[config.asset_id] = params;

    return errors::OK;
}

int32_t LXOracle::update_config(uint64_t asset_id, const OracleConfig& config) {
    std::unique_lock lock(config_mutex_);

    auto it = configs_.find(asset_id);
    if (it == configs_.end()) {
        return errors::MARKET_NOT_FOUND;
    }

    it->second = config;
    return errors::OK;
}

std::optional<OracleConfig> LXOracle::get_config(uint64_t asset_id) const {
    std::shared_lock lock(config_mutex_);
    auto it = configs_.find(asset_id);
    if (it == configs_.end()) return std::nullopt;
    return it->second;
}

void LXOracle::set_robust_params(uint64_t asset_id, const RobustParams& params) {
    std::unique_lock lock(config_mutex_);
    robust_params_[asset_id] = params;
}

std::optional<RobustParams> LXOracle::get_robust_params(uint64_t asset_id) const {
    std::shared_lock lock(config_mutex_);
    auto it = robust_params_.find(asset_id);
    if (it == robust_params_.end()) return std::nullopt;
    return it->second;
}

// =============================================================================
// Price Updates
// =============================================================================

int32_t LXOracle::update_price(uint64_t asset_id, PriceSource source,
                                I128 price_x18, I128 confidence_x18,
                                uint64_t timestamp) {
    if (price_x18 <= 0) {
        return errors::INVALID_PRICE;
    }

    if (timestamp == 0) {
        timestamp = current_timestamp();
    }

    std::unique_lock lock(prices_mutex_);

    SourcePriceData data;
    data.source = source;
    data.price_x18 = price_x18;
    data.confidence_x18 = confidence_x18;
    data.timestamp = timestamp;
    data.block_number = 0; // Would be set from context
    data.is_valid = true;

    prices_[asset_id][static_cast<uint8_t>(source)] = data;

    total_updates_.fetch_add(1, std::memory_order_relaxed);

    return errors::OK;
}

int32_t LXOracle::update_prices(const std::vector<std::tuple<uint64_t, PriceSource, I128, I128>>& updates) {
    uint64_t timestamp = current_timestamp();

    std::unique_lock lock(prices_mutex_);

    for (const auto& [asset_id, source, price, confidence] : updates) {
        if (price <= 0) continue;

        SourcePriceData data;
        data.source = source;
        data.price_x18 = price;
        data.confidence_x18 = confidence;
        data.timestamp = timestamp;
        data.block_number = 0;
        data.is_valid = true;

        prices_[asset_id][static_cast<uint8_t>(source)] = data;
    }

    total_updates_.fetch_add(updates.size(), std::memory_order_relaxed);

    return errors::OK;
}

// =============================================================================
// Price Queries
// =============================================================================

std::optional<I128> LXOracle::get_price(uint64_t asset_id) const {
    auto data = get_price_data(asset_id);
    if (!data) return std::nullopt;
    return data->price_x18;
}

std::optional<AggregatedPriceData> LXOracle::get_price_data(uint64_t asset_id) const {
    std::shared_lock config_lock(config_mutex_);
    auto config_it = configs_.find(asset_id);
    if (config_it == configs_.end()) return std::nullopt;

    const OracleConfig& config = config_it->second;
    config_lock.unlock();

    std::shared_lock price_lock(prices_mutex_);
    auto asset_it = prices_.find(asset_id);
    if (asset_it == prices_.end()) return std::nullopt;

    // Collect valid prices from all sources
    std::vector<I128> valid_prices;
    uint64_t latest_timestamp = 0;
    uint64_t now = current_timestamp();

    for (const auto& [source_id, data] : asset_it->second) {
        // Check staleness
        if (now - data.timestamp > config.max_staleness) {
            continue;
        }
        if (!data.is_valid) continue;

        valid_prices.push_back(data.price_x18);
        if (data.timestamp > latest_timestamp) {
            latest_timestamp = data.timestamp;
        }
    }

    if (valid_prices.empty()) return std::nullopt;

    // Aggregate based on method
    I128 aggregated_price;
    switch (config.method) {
        case AggregationMethod::MEDIAN:
            aggregated_price = aggregate_median(valid_prices);
            break;
        case AggregationMethod::TWAP:
        case AggregationMethod::VWAP:
        case AggregationMethod::TRIMMED_MEAN: {
            config_lock.lock();
            auto robust_it = robust_params_.find(asset_id);
            I128 trim = (robust_it != robust_params_.end()) ?
                robust_it->second.trim_percent_x18 : x18::from_double(0.1);
            config_lock.unlock();
            aggregated_price = aggregate_trimmed_mean(valid_prices, trim);
            break;
        }
        case AggregationMethod::WEIGHTED_MEDIAN:
            aggregated_price = aggregate_weighted_median(valid_prices, config.weights_x18);
            break;
        default:
            aggregated_price = aggregate_mean(valid_prices);
    }

    // Calculate confidence and deviation
    I128 mean = aggregate_mean(valid_prices);
    I128 variance = 0;
    for (I128 p : valid_prices) {
        I128 diff = p - mean;
        variance += x18::mul(diff, diff);
    }
    if (valid_prices.size() > 1) {
        variance /= static_cast<I128>(valid_prices.size() - 1);
    }
    I128 std_dev = x18::sqrt(variance);

    AggregatedPriceData result;
    result.price_x18 = aggregated_price;
    result.confidence_x18 = std_dev; // Use std dev as confidence
    result.deviation_x18 = std_dev;
    result.num_sources = static_cast<uint8_t>(valid_prices.size());
    result.timestamp = latest_timestamp;
    result.method = config.method;

    return result;
}

std::vector<std::pair<uint64_t, I128>> LXOracle::get_prices(const std::vector<uint64_t>& asset_ids) const {
    std::vector<std::pair<uint64_t, I128>> results;
    results.reserve(asset_ids.size());

    for (uint64_t id : asset_ids) {
        auto price = get_price(id);
        if (price) {
            results.emplace_back(id, *price);
        }
    }

    return results;
}

std::optional<SourcePriceData> LXOracle::get_source_price(uint64_t asset_id, PriceSource source) const {
    std::shared_lock lock(prices_mutex_);

    auto asset_it = prices_.find(asset_id);
    if (asset_it == prices_.end()) return std::nullopt;

    auto source_it = asset_it->second.find(static_cast<uint8_t>(source));
    if (source_it == asset_it->second.end()) return std::nullopt;

    return source_it->second;
}

std::vector<SourcePriceData> LXOracle::get_all_source_prices(uint64_t asset_id) const {
    std::vector<SourcePriceData> results;

    std::shared_lock lock(prices_mutex_);
    auto asset_it = prices_.find(asset_id);
    if (asset_it == prices_.end()) return results;

    for (const auto& [source_id, data] : asset_it->second) {
        results.push_back(data);
    }

    return results;
}

// =============================================================================
// Index Price (Robust Construction)
// =============================================================================

std::optional<I128> LXOracle::index_price(uint64_t asset_id) const {
    auto detail = index_price_detailed(asset_id);
    if (!detail) return std::nullopt;
    return detail->price_x18;
}

std::optional<LXOracle::IndexPriceDetail> LXOracle::index_price_detailed(uint64_t asset_id) const {
    std::shared_lock config_lock(config_mutex_);
    auto robust_it = robust_params_.find(asset_id);
    RobustParams params;
    if (robust_it != robust_params_.end()) {
        params = robust_it->second;
    } else {
        params.min_sources = 1;
        params.outlier_threshold_x18 = x18::from_double(3.0);
        params.trim_percent_x18 = x18::from_double(0.1);
        params.use_volume_weighting = false;
    }
    config_lock.unlock();

    auto sources = get_all_source_prices(asset_id);
    if (sources.size() < params.min_sources) {
        return std::nullopt;
    }

    // Collect valid prices
    std::vector<I128> prices;
    std::vector<PriceSource> source_types;
    uint64_t now = current_timestamp();

    std::shared_lock cfg_lock(config_mutex_);
    auto config_it = configs_.find(asset_id);
    uint64_t max_staleness = (config_it != configs_.end()) ?
        config_it->second.max_staleness : 60;
    cfg_lock.unlock();

    for (const auto& data : sources) {
        if (!data.is_valid) continue;
        if (now - data.timestamp > max_staleness) continue;

        prices.push_back(data.price_x18);
        source_types.push_back(data.source);
    }

    if (prices.size() < params.min_sources) {
        return std::nullopt;
    }

    // Calculate median and mean
    I128 median = aggregate_median(prices);
    I128 mean = aggregate_mean(prices);

    // Detect outliers
    std::vector<bool> is_outlier = detect_outliers(prices, params.outlier_threshold_x18);

    // Filter outliers
    std::vector<I128> filtered_prices;
    std::vector<PriceSource> filtered_sources;
    uint8_t outliers_count = 0;

    for (size_t i = 0; i < prices.size(); ++i) {
        if (is_outlier[i]) {
            outliers_count++;
            filtered_sources.push_back(source_types[i]);
        } else {
            filtered_prices.push_back(prices[i]);
        }
    }

    if (filtered_prices.empty()) {
        // All prices were outliers - fall back to median
        filtered_prices = prices;
    }

    // Calculate final index price
    I128 index = aggregate_trimmed_mean(filtered_prices, params.trim_percent_x18);

    // Calculate std dev
    I128 variance = 0;
    for (I128 p : filtered_prices) {
        I128 diff = p - mean;
        variance += x18::mul(diff, diff);
    }
    if (filtered_prices.size() > 1) {
        variance /= static_cast<I128>(filtered_prices.size() - 1);
    }
    I128 std_dev = x18::sqrt(variance);

    IndexPriceDetail result;
    result.price_x18 = index;
    result.median_x18 = median;
    result.mean_x18 = mean;
    result.std_dev_x18 = std_dev;
    result.sources_used = static_cast<uint8_t>(filtered_prices.size());
    result.outliers_filtered = outliers_count;
    result.filtered_sources = filtered_sources;

    return result;
}

// =============================================================================
// TWAP Interface
// =============================================================================

std::optional<I128> LXOracle::get_twap(uint64_t asset_id, uint64_t window_seconds) const {
    std::shared_lock lock(twap_mutex_);

    auto it = twap_data_.find(asset_id);
    if (it == twap_data_.end() || it->second.empty()) {
        return std::nullopt;
    }

    uint64_t now = current_timestamp();
    uint64_t cutoff = now - window_seconds;

    I128 sum = 0;
    I128 total_weight = 0;
    I128 prev_time = cutoff;

    for (const auto& [timestamp, price] : it->second) {
        if (timestamp < cutoff) continue;

        I128 weight = static_cast<I128>(timestamp - prev_time);
        sum += x18::mul(price, weight);
        total_weight += weight;
        prev_time = timestamp;
    }

    // Add weight for time until now
    if (!it->second.empty() && it->second.back().first >= cutoff) {
        I128 final_weight = static_cast<I128>(now - prev_time);
        sum += x18::mul(it->second.back().second, final_weight);
        total_weight += final_weight;
    }

    if (total_weight == 0) return std::nullopt;

    return x18::div(sum, total_weight);
}

void LXOracle::record_twap_price(uint64_t asset_id, I128 price_x18, uint64_t timestamp) {
    if (timestamp == 0) {
        timestamp = current_timestamp();
    }

    std::unique_lock lock(twap_mutex_);

    auto& history = twap_data_[asset_id];
    history.emplace_back(timestamp, price_x18);

    // Keep only last 24 hours of data
    constexpr uint64_t MAX_HISTORY = 24 * 3600;
    uint64_t cutoff = timestamp > MAX_HISTORY ? timestamp - MAX_HISTORY : 0;

    auto new_begin = std::lower_bound(history.begin(), history.end(), cutoff,
        [](const auto& p, uint64_t t) { return p.first < t; });

    if (new_begin != history.begin()) {
        history.erase(history.begin(), new_begin);
    }
}

// =============================================================================
// Staleness & Validity
// =============================================================================

bool LXOracle::is_price_fresh(uint64_t asset_id) const {
    std::shared_lock lock(config_mutex_);
    auto config_it = configs_.find(asset_id);
    uint64_t max_staleness = (config_it != configs_.end()) ?
        config_it->second.max_staleness : 60;
    lock.unlock();

    return is_price_fresh(asset_id, max_staleness);
}

bool LXOracle::is_price_fresh(uint64_t asset_id, uint64_t max_staleness) const {
    return price_age(asset_id) <= max_staleness;
}

uint64_t LXOracle::price_age(uint64_t asset_id) const {
    std::shared_lock lock(prices_mutex_);

    auto asset_it = prices_.find(asset_id);
    if (asset_it == prices_.end()) return UINT64_MAX;

    uint64_t latest = 0;
    for (const auto& [source_id, data] : asset_it->second) {
        if (data.timestamp > latest) {
            latest = data.timestamp;
        }
    }

    uint64_t now = current_timestamp();
    return now > latest ? now - latest : 0;
}

// =============================================================================
// Statistics
// =============================================================================

LXOracle::Stats LXOracle::get_stats() const {
    std::shared_lock config_lock(config_mutex_);
    std::shared_lock price_lock(prices_mutex_);

    uint64_t stale_count = 0;
    uint64_t now = current_timestamp();

    for (const auto& [asset_id, sources] : prices_) {
        auto config_it = configs_.find(asset_id);
        uint64_t max_staleness = (config_it != configs_.end()) ?
            config_it->second.max_staleness : 60;

        bool has_fresh = false;
        for (const auto& [source_id, data] : sources) {
            if (now - data.timestamp <= max_staleness) {
                has_fresh = true;
                break;
            }
        }
        if (!has_fresh) stale_count++;
    }

    return Stats{
        configs_.size(),
        total_updates_.load(std::memory_order_relaxed),
        stale_count
    };
}

// =============================================================================
// Internal Helpers - Aggregation
// =============================================================================

I128 LXOracle::aggregate_median(const std::vector<I128>& prices) const {
    if (prices.empty()) return 0;

    std::vector<I128> sorted = prices;
    std::sort(sorted.begin(), sorted.end());

    size_t n = sorted.size();
    if (n % 2 == 1) {
        return sorted[n / 2];
    } else {
        return (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
    }
}

I128 LXOracle::aggregate_mean(const std::vector<I128>& prices) const {
    if (prices.empty()) return 0;

    I128 sum = 0;
    for (I128 p : prices) {
        sum += p;
    }
    return sum / static_cast<I128>(prices.size());
}

I128 LXOracle::aggregate_trimmed_mean(const std::vector<I128>& prices, I128 trim_percent_x18) const {
    if (prices.empty()) return 0;
    if (prices.size() <= 2) return aggregate_mean(prices);

    std::vector<I128> sorted = prices;
    std::sort(sorted.begin(), sorted.end());

    size_t n = sorted.size();
    size_t trim_count = static_cast<size_t>(x18::to_double(trim_percent_x18) * n);
    trim_count = std::min(trim_count, n / 2 - 1);

    I128 sum = 0;
    size_t count = 0;
    for (size_t i = trim_count; i < n - trim_count; ++i) {
        sum += sorted[i];
        count++;
    }

    return count > 0 ? sum / static_cast<I128>(count) : 0;
}

I128 LXOracle::aggregate_weighted_median(const std::vector<I128>& prices,
                                          const std::vector<I128>& weights) const {
    if (prices.empty()) return 0;
    if (weights.size() != prices.size()) return aggregate_median(prices);

    // Sort by price, keeping track of weights
    std::vector<std::pair<I128, I128>> price_weight;
    for (size_t i = 0; i < prices.size(); ++i) {
        price_weight.emplace_back(prices[i], weights[i]);
    }
    std::sort(price_weight.begin(), price_weight.end());

    // Find weighted median
    I128 total_weight = 0;
    for (const auto& [p, w] : price_weight) {
        total_weight += w;
    }

    I128 half_weight = total_weight / 2;
    I128 cumulative = 0;

    for (const auto& [price, weight] : price_weight) {
        cumulative += weight;
        if (cumulative >= half_weight) {
            return price;
        }
    }

    return price_weight.back().first;
}

// =============================================================================
// Internal Helpers - Outlier Detection
// =============================================================================

std::vector<bool> LXOracle::detect_outliers(const std::vector<I128>& prices, I128 threshold_x18) const {
    std::vector<bool> outliers(prices.size(), false);

    if (prices.size() < 3) return outliers;

    I128 mean = aggregate_mean(prices);

    // Calculate standard deviation
    I128 variance = 0;
    for (I128 p : prices) {
        I128 diff = p - mean;
        variance += x18::mul(diff, diff);
    }
    variance /= static_cast<I128>(prices.size());
    I128 std_dev = x18::sqrt(variance);

    if (std_dev == 0) return outliers;

    // Mark outliers using z-score
    for (size_t i = 0; i < prices.size(); ++i) {
        I128 z_score = x18::div(prices[i] - mean, std_dev);
        if (z_score < 0) z_score = -z_score;

        if (z_score > threshold_x18) {
            outliers[i] = true;
        }
    }

    return outliers;
}

uint64_t LXOracle::current_timestamp() const {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

// =============================================================================
// Oracle Adapters (Stubs - would connect to actual sources)
// =============================================================================

ChainlinkAdapter::ChainlinkAdapter(const Address& registry) : registry_(registry) {}

bool ChainlinkAdapter::is_available() const {
    // Check if Chainlink registry is reachable
    return true;
}

std::optional<SourcePriceData> ChainlinkAdapter::fetch_price(uint64_t asset_id) {
    // Would call Chainlink aggregator contract
    return std::nullopt;
}

std::vector<std::pair<uint64_t, SourcePriceData>>
ChainlinkAdapter::fetch_prices(const std::vector<uint64_t>& asset_ids) {
    // Batch fetch from Chainlink
    return {};
}

PythAdapter::PythAdapter(const Address& pyth_contract) : pyth_contract_(pyth_contract) {}

bool PythAdapter::is_available() const {
    return true;
}

std::optional<SourcePriceData> PythAdapter::fetch_price(uint64_t asset_id) {
    // Would call Pyth contract
    return std::nullopt;
}

std::vector<std::pair<uint64_t, SourcePriceData>>
PythAdapter::fetch_prices(const std::vector<uint64_t>& asset_ids) {
    return {};
}

std::optional<SourcePriceData> LXPoolAdapter::fetch_price(uint64_t asset_id) {
    // Would query LXPool for spot price
    return std::nullopt;
}

std::vector<std::pair<uint64_t, SourcePriceData>>
LXPoolAdapter::fetch_prices(const std::vector<uint64_t>& asset_ids) {
    return {};
}

} // namespace lux
