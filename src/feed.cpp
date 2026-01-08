// =============================================================================
// feed.cpp - LXFeed Computed Price Feeds Implementation
// =============================================================================

#include "lux/feed.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>

namespace lux {

// =============================================================================
// Constructor
// =============================================================================

LXFeed::LXFeed(LXOracle& oracle) : oracle_(oracle) {}

// =============================================================================
// Configuration
// =============================================================================

void LXFeed::set_mark_price_config(uint32_t market_id, const MarkPriceConfig& config) {
    std::unique_lock lock(config_mutex_);
    mark_configs_[market_id] = config;
}

std::optional<MarkPriceConfig> LXFeed::get_mark_price_config(uint32_t market_id) const {
    std::shared_lock lock(config_mutex_);
    auto it = mark_configs_.find(market_id);
    if (it == mark_configs_.end()) return std::nullopt;
    return it->second;
}

void LXFeed::set_funding_params(uint32_t market_id, const FundingParams& params) {
    std::unique_lock lock(config_mutex_);
    funding_params_[market_id] = params;
}

std::optional<FundingParams> LXFeed::get_funding_params(uint32_t market_id) const {
    std::shared_lock lock(config_mutex_);
    auto it = funding_params_.find(market_id);
    if (it == funding_params_.end()) return std::nullopt;
    return it->second;
}

void LXFeed::set_trigger_rules(uint32_t market_id, const std::vector<TriggerRule>& rules) {
    std::unique_lock lock(config_mutex_);
    trigger_rules_[market_id] = rules;
}

// =============================================================================
// Index Price
// =============================================================================

std::optional<I128> LXFeed::index_price(uint32_t market_id) const {
    std::shared_lock lock(market_mutex_);
    auto it = market_assets_.find(market_id);
    if (it == market_assets_.end()) return std::nullopt;

    return oracle_.index_price(it->second);
}

std::optional<std::pair<I128, uint64_t>> LXFeed::index_price_with_time(uint32_t market_id) const {
    std::shared_lock lock(market_mutex_);
    auto it = market_assets_.find(market_id);
    if (it == market_assets_.end()) return std::nullopt;

    auto price_data = oracle_.get_price_data(it->second);
    if (!price_data) return std::nullopt;

    return std::make_pair(price_data->price_x18, price_data->timestamp);
}

// =============================================================================
// Mark Price
// =============================================================================

std::optional<I128> LXFeed::mark_price(uint32_t market_id) const {
    auto mark_data = get_mark_price(market_id);
    if (!mark_data) return std::nullopt;
    return mark_data->mark_px_x18;
}

std::optional<LXMarkPrice> LXFeed::get_mark_price(uint32_t market_id) const {
    // Get index price
    auto index = index_price(market_id);
    if (!index) return std::nullopt;

    // Get premium EWMA
    auto prem = premium_ewma(market_id);
    I128 premium = prem.value_or(0);

    // Get config for clamping
    std::shared_lock lock(config_mutex_);
    auto config_it = mark_configs_.find(market_id);
    if (config_it != mark_configs_.end()) {
        const MarkPriceConfig& config = config_it->second;
        // Clamp premium
        if (premium > config.max_premium_x18) {
            premium = config.max_premium_x18;
        } else if (premium < config.min_premium_x18) {
            premium = config.min_premium_x18;
        }
    }
    lock.unlock();

    LXMarkPrice result;
    result.index_px_x18 = *index;
    result.premium_x18 = premium;
    result.mark_px_x18 = *index + premium;
    result.timestamp = current_timestamp();

    return result;
}

// =============================================================================
// Last Trade Price
// =============================================================================

std::optional<I128> LXFeed::last_price(uint32_t market_id) const {
    std::shared_lock lock(price_mutex_);
    const MarketPriceState* state = get_price_state(market_id);
    if (!state || state->last_price_x18 == 0) return std::nullopt;
    return state->last_price_x18;
}

void LXFeed::update_last_price(uint32_t market_id, I128 price_x18, uint64_t timestamp) {
    if (timestamp == 0) {
        timestamp = current_timestamp();
    }

    std::unique_lock lock(price_mutex_);
    MarketPriceState* state = get_price_state(market_id);
    if (!state) {
        price_states_[market_id] = MarketPriceState{};
        state = &price_states_[market_id];
    }

    state->last_price_x18 = price_x18;
    state->last_price_time = timestamp;

    total_price_updates_.fetch_add(1, std::memory_order_relaxed);
}

// =============================================================================
// Mid Price
// =============================================================================

std::optional<I128> LXFeed::mid_price(uint32_t market_id) const {
    std::shared_lock lock(price_mutex_);
    const MarketPriceState* state = get_price_state(market_id);
    if (!state) return std::nullopt;

    if (state->best_bid_x18 == 0 || state->best_ask_x18 == 0) {
        return std::nullopt;
    }

    return (state->best_bid_x18 + state->best_ask_x18) / 2;
}

void LXFeed::update_bbo(uint32_t market_id, I128 best_bid_x18, I128 best_ask_x18) {
    std::unique_lock lock(price_mutex_);
    MarketPriceState* state = get_price_state(market_id);
    if (!state) {
        price_states_[market_id] = MarketPriceState{};
        state = &price_states_[market_id];
    }

    state->best_bid_x18 = best_bid_x18;
    state->best_ask_x18 = best_ask_x18;
}

// =============================================================================
// Generic Price Query
// =============================================================================

std::optional<I128> LXFeed::get_price(uint32_t market_id, PriceType type) const {
    switch (type) {
        case PriceType::INDEX:
            return index_price(market_id);
        case PriceType::MARK:
            return mark_price(market_id);
        case PriceType::LAST:
            return last_price(market_id);
        case PriceType::MID:
            return mid_price(market_id);
        case PriceType::ORACLE: {
            std::shared_lock lock(market_mutex_);
            auto it = market_assets_.find(market_id);
            if (it == market_assets_.end()) return std::nullopt;
            return oracle_.get_price(it->second);
        }
        default:
            return std::nullopt;
    }
}

std::optional<LXFeed::AllPrices> LXFeed::get_all_prices(uint32_t market_id) const {
    AllPrices prices{};

    auto idx = index_price(market_id);
    auto mrk = mark_price(market_id);
    auto lst = last_price(market_id);
    auto mid = mid_price(market_id);

    if (!idx && !mrk && !lst && !mid) {
        return std::nullopt;
    }

    prices.index_x18 = idx.value_or(0);
    prices.mark_x18 = mrk.value_or(0);
    prices.last_x18 = lst.value_or(0);
    prices.mid_x18 = mid.value_or(0);
    prices.timestamp = current_timestamp();

    return prices;
}

std::vector<std::pair<uint32_t, LXFeed::AllPrices>>
LXFeed::get_multiple_market_prices(const std::vector<uint32_t>& market_ids) const {
    std::vector<std::pair<uint32_t, AllPrices>> results;
    results.reserve(market_ids.size());

    for (uint32_t id : market_ids) {
        auto prices = get_all_prices(id);
        if (prices) {
            results.emplace_back(id, *prices);
        }
    }

    return results;
}

// =============================================================================
// Premium & Basis
// =============================================================================

std::optional<I128> LXFeed::premium(uint32_t market_id) const {
    auto idx = index_price(market_id);
    auto mrk = mark_price(market_id);

    if (!idx || !mrk) return std::nullopt;

    return *mrk - *idx;
}

std::optional<I128> LXFeed::basis(uint32_t market_id) const {
    auto idx = index_price(market_id);
    auto prem = premium(market_id);

    if (!idx || !prem || *idx == 0) return std::nullopt;

    return x18::div(*prem, *idx);
}

std::optional<I128> LXFeed::premium_ewma(uint32_t market_id) const {
    std::shared_lock lock(price_mutex_);
    const MarketPriceState* state = get_price_state(market_id);
    if (!state) return std::nullopt;
    return state->premium_ewma_x18;
}

void LXFeed::record_premium(uint32_t market_id, I128 premium_x18, uint64_t timestamp) {
    if (timestamp == 0) {
        timestamp = current_timestamp();
    }

    std::unique_lock lock(price_mutex_);
    MarketPriceState* state = get_price_state(market_id);
    if (!state) {
        price_states_[market_id] = MarketPriceState{};
        state = &price_states_[market_id];
    }

    state->premium_history.emplace_back(timestamp, premium_x18);

    // Keep only last hour of premium data
    constexpr uint64_t MAX_HISTORY = 3600;
    uint64_t cutoff = timestamp > MAX_HISTORY ? timestamp - MAX_HISTORY : 0;

    auto new_begin = std::lower_bound(state->premium_history.begin(),
                                       state->premium_history.end(), cutoff,
        [](const auto& p, uint64_t t) { return p.first < t; });

    if (new_begin != state->premium_history.begin()) {
        state->premium_history.erase(state->premium_history.begin(), new_begin);
    }

    // Update EWMA
    lock.unlock();
    std::shared_lock config_lock(config_mutex_);
    auto config_it = mark_configs_.find(market_id);
    uint64_t window = (config_it != mark_configs_.end()) ?
        config_it->second.premium_ewma_window : 300; // Default 5 minutes
    config_lock.unlock();

    lock.lock();
    state->premium_ewma_x18 = calculate_ewma(state->premium_history, window, timestamp);
}

// =============================================================================
// Funding Rate
// =============================================================================

std::optional<I128> LXFeed::funding_rate(uint32_t market_id) const {
    std::shared_lock lock(price_mutex_);
    const MarketPriceState* state = get_price_state(market_id);
    if (!state) return std::nullopt;
    return state->current_funding_rate_x18;
}

std::optional<LXFundingRate> LXFeed::get_funding_rate(uint32_t market_id) const {
    std::shared_lock lock(price_mutex_);
    const MarketPriceState* state = get_price_state(market_id);
    if (!state) return std::nullopt;

    LXFundingRate result;
    result.rate_x18 = state->current_funding_rate_x18;
    result.next_funding_time = state->next_funding_time;

    return result;
}

uint64_t LXFeed::funding_interval(uint32_t market_id) const {
    std::shared_lock lock(config_mutex_);
    auto it = funding_params_.find(market_id);
    if (it == funding_params_.end()) return 28800; // Default 8 hours
    return it->second.funding_interval;
}

I128 LXFeed::max_funding_rate(uint32_t market_id) const {
    std::shared_lock lock(config_mutex_);
    auto it = funding_params_.find(market_id);
    if (it == funding_params_.end()) return x18::from_double(0.01); // Default 1%
    return it->second.max_funding_rate_x18;
}

std::optional<I128> LXFeed::predicted_funding_rate(uint32_t market_id) const {
    // Calculate what funding rate would be at next interval
    std::shared_lock lock(price_mutex_);
    const MarketPriceState* state = get_price_state(market_id);
    if (!state) return std::nullopt;

    std::shared_lock config_lock(config_mutex_);
    auto params_it = funding_params_.find(market_id);
    FundingParams params;
    if (params_it != funding_params_.end()) {
        params = params_it->second;
    } else {
        params.funding_interval = 28800;
        params.max_funding_rate_x18 = x18::from_double(0.01);
        params.interest_rate_x18 = x18::from_double(0.0001); // 0.01%
        params.premium_fraction_x18 = X18_ONE;
        params.use_twap_premium = true;
    }
    config_lock.unlock();

    return compute_funding_rate(*state, params);
}

void LXFeed::calculate_funding_rate(uint32_t market_id) {
    std::unique_lock lock(price_mutex_);
    MarketPriceState* state = get_price_state(market_id);
    if (!state) {
        price_states_[market_id] = MarketPriceState{};
        state = &price_states_[market_id];
    }

    std::shared_lock config_lock(config_mutex_);
    auto params_it = funding_params_.find(market_id);
    FundingParams params;
    if (params_it != funding_params_.end()) {
        params = params_it->second;
    } else {
        params.funding_interval = 28800;
        params.max_funding_rate_x18 = x18::from_double(0.01);
        params.interest_rate_x18 = x18::from_double(0.0001);
        params.premium_fraction_x18 = X18_ONE;
        params.use_twap_premium = true;
    }
    config_lock.unlock();

    state->current_funding_rate_x18 = compute_funding_rate(*state, params);
    state->last_funding_calc_time = current_timestamp();
    state->next_funding_time = state->last_funding_calc_time + params.funding_interval;

    funding_calculations_.fetch_add(1, std::memory_order_relaxed);
}

// =============================================================================
// Trigger Price
// =============================================================================

std::optional<I128> LXFeed::get_trigger_price(uint32_t market_id, bool is_buy) const {
    // For triggers, use last trade price by default
    auto last = last_price(market_id);
    if (last) return last;

    // Fall back to mark price
    return mark_price(market_id);
}

bool LXFeed::check_trigger(uint32_t market_id, TriggerType type,
                           bool is_buy, I128 trigger_price_x18) const {
    std::optional<I128> current;

    switch (type) {
        case TriggerType::STOP_LOSS:
        case TriggerType::TAKE_PROFIT:
            current = last_price(market_id);
            if (!current) current = mark_price(market_id);
            break;

        case TriggerType::LIQUIDATION:
            current = mark_price(market_id);
            break;

        case TriggerType::FUNDING:
        case TriggerType::ADL:
            // These don't use price triggers
            return false;
    }

    if (!current) return false;

    // Check trigger condition
    if (is_buy) {
        // Buy triggers activate when price drops to/below trigger
        return *current <= trigger_price_x18;
    } else {
        // Sell triggers activate when price rises to/above trigger
        return *current >= trigger_price_x18;
    }
}

std::optional<I128> LXFeed::liquidation_price(uint32_t market_id, const LXPosition& position,
                                               I128 maintenance_margin_x18) const {
    if (position.size_x18 == 0) return std::nullopt;

    // liq_price = entry - (mm * notional / size) for longs
    // liq_price = entry + (mm * notional / size) for shorts

    I128 size_abs = position.size_x18 > 0 ? position.size_x18 : -position.size_x18;
    I128 notional = x18::mul(size_abs, position.entry_px_x18);
    I128 mm_value = x18::mul(notional, maintenance_margin_x18);
    I128 buffer = x18::div(mm_value, size_abs);

    if (position.side == PositionSide::LONG) {
        return position.entry_px_x18 - buffer;
    } else {
        return position.entry_px_x18 + buffer;
    }
}

// =============================================================================
// Market Registration
// =============================================================================

int32_t LXFeed::register_market(uint32_t market_id, uint64_t asset_id) {
    std::unique_lock lock(market_mutex_);

    if (market_assets_.find(market_id) != market_assets_.end()) {
        return errors::POOL_ALREADY_INITIALIZED;
    }

    market_assets_[market_id] = asset_id;

    // Initialize price state
    std::unique_lock price_lock(price_mutex_);
    price_states_[market_id] = MarketPriceState{};

    return errors::OK;
}

void LXFeed::unregister_market(uint32_t market_id) {
    std::unique_lock lock(market_mutex_);
    market_assets_.erase(market_id);

    std::unique_lock price_lock(price_mutex_);
    price_states_.erase(market_id);

    std::unique_lock config_lock(config_mutex_);
    mark_configs_.erase(market_id);
    funding_params_.erase(market_id);
    trigger_rules_.erase(market_id);
}

bool LXFeed::market_exists(uint32_t market_id) const {
    std::shared_lock lock(market_mutex_);
    return market_assets_.find(market_id) != market_assets_.end();
}

// =============================================================================
// Statistics
// =============================================================================

LXFeed::Stats LXFeed::get_stats() const {
    std::shared_lock lock(market_mutex_);
    return Stats{
        market_assets_.size(),
        total_price_updates_.load(std::memory_order_relaxed),
        funding_calculations_.load(std::memory_order_relaxed)
    };
}

// =============================================================================
// Internal Helpers
// =============================================================================

LXFeed::MarketPriceState* LXFeed::get_price_state(uint32_t market_id) {
    auto it = price_states_.find(market_id);
    return (it != price_states_.end()) ? &it->second : nullptr;
}

const LXFeed::MarketPriceState* LXFeed::get_price_state(uint32_t market_id) const {
    auto it = price_states_.find(market_id);
    return (it != price_states_.end()) ? &it->second : nullptr;
}

I128 LXFeed::calculate_ewma(const std::vector<std::pair<uint64_t, I128>>& history,
                             uint64_t window_seconds, uint64_t current_time) const {
    if (history.empty()) return 0;

    // EWMA with decay factor based on time
    double decay = 2.0 / (static_cast<double>(window_seconds) + 1.0);
    double ewma = 0.0;
    double weight_sum = 0.0;
    uint64_t prev_time = current_time - window_seconds;

    for (const auto& [timestamp, value] : history) {
        if (timestamp < current_time - window_seconds) continue;

        double age = static_cast<double>(current_time - timestamp);
        double weight = std::exp(-decay * age / static_cast<double>(window_seconds));

        ewma += weight * x18::to_double(value);
        weight_sum += weight;
    }

    if (weight_sum == 0) return 0;

    return x18::from_double(ewma / weight_sum);
}

I128 LXFeed::compute_funding_rate(const MarketPriceState& state,
                                   const FundingParams& params) const {
    // funding_rate = clamp(premium * fraction + interest_rate, -max, +max)

    I128 premium_component = x18::mul(state.premium_ewma_x18, params.premium_fraction_x18);
    I128 rate = premium_component + params.interest_rate_x18;

    // Clamp to max
    if (rate > params.max_funding_rate_x18) {
        rate = params.max_funding_rate_x18;
    } else if (rate < -params.max_funding_rate_x18) {
        rate = -params.max_funding_rate_x18;
    }

    return rate;
}

uint64_t LXFeed::current_timestamp() const {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

} // namespace lux
