// =============================================================================
// vault.cpp - LXVault Clearinghouse Implementation
// =============================================================================

#include "lux/vault.hpp"
#include <chrono>
#include <algorithm>

namespace lux {

// =============================================================================
// Constructor
// =============================================================================

LXVault::LXVault() = default;

// =============================================================================
// Market Management
// =============================================================================

int32_t LXVault::create_market(const MarketConfig& config) {
    std::unique_lock lock(markets_mutex_);

    if (markets_.find(config.market_id) != markets_.end()) {
        return errors::POOL_ALREADY_INITIALIZED;
    }

    markets_[config.market_id] = config;

    // Initialize funding state
    std::unique_lock funding_lock(funding_mutex_);
    FundingState funding;
    funding.current_rate_x18 = 0;
    funding.cumulative_funding_x18 = 0;
    funding.last_funding_time = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    funding.funding_interval = 28800; // 8 hours
    funding_[config.market_id] = funding;

    return errors::OK;
}

int32_t LXVault::update_market(const MarketConfig& config) {
    std::unique_lock lock(markets_mutex_);

    auto it = markets_.find(config.market_id);
    if (it == markets_.end()) {
        return errors::MARKET_NOT_FOUND;
    }

    it->second = config;
    return errors::OK;
}

std::optional<MarketConfig> LXVault::get_market_config(uint32_t market_id) const {
    std::shared_lock lock(markets_mutex_);
    auto it = markets_.find(market_id);
    if (it == markets_.end()) return std::nullopt;
    return it->second;
}

bool LXVault::market_exists(uint32_t market_id) const {
    std::shared_lock lock(markets_mutex_);
    return markets_.find(market_id) != markets_.end();
}

// =============================================================================
// Deposit/Withdraw
// =============================================================================

int32_t LXVault::deposit(const LXAccount& account, const Currency& token, I128 amount_x18) {
    if (amount_x18 <= 0) {
        return errors::INVALID_PRICE;
    }

    uint64_t currency_hash = 0;
    for (auto b : token.addr) currency_hash = currency_hash * 31 + b;

    std::unique_lock lock(accounts_mutex_);
    AccountState* state = get_or_create_account(account);
    state->balances[currency_hash] += amount_x18;
    state->last_update_time = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    return errors::OK;
}

int32_t LXVault::withdraw(const LXAccount& account, const Currency& token, I128 amount_x18) {
    if (amount_x18 <= 0) {
        return errors::INVALID_PRICE;
    }

    uint64_t currency_hash = 0;
    for (auto b : token.addr) currency_hash = currency_hash * 31 + b;

    // FIX: Hold lock through entire operation to prevent TOCTOU race.
    // Previously margin check was done without lock, then lock acquired.
    std::unique_lock accounts_lock(accounts_mutex_);
    std::shared_lock markets_lock(markets_mutex_);

    AccountState* state = get_or_create_account(account);

    // Check balance exists
    auto it = state->balances.find(currency_hash);
    if (it == state->balances.end() || it->second < amount_x18) {
        return errors::INSUFFICIENT_BALANCE;
    }

    // Calculate margin info inline while holding lock
    I128 total_collateral = 0;
    for (const auto& [hash, bal] : state->balances) {
        total_collateral += bal;
    }

    I128 total_unrealized_pnl = 0;
    I128 total_initial_margin = 0;
    for (const auto& [market_id, position] : state->positions) {
        auto config_it = markets_.find(market_id);
        if (config_it == markets_.end()) continue;
        total_unrealized_pnl += position.unrealized_pnl_x18;
        total_initial_margin += calculate_initial_margin(position, config_it->second);
    }

    I128 equity = total_collateral + total_unrealized_pnl;
    I128 free_margin = equity - total_initial_margin;

    if (free_margin < amount_x18) {
        return errors::INSUFFICIENT_MARGIN;
    }

    // Perform withdrawal atomically
    state->balances[currency_hash] -= amount_x18;
    state->last_update_time = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    return errors::OK;
}

int32_t LXVault::transfer(const LXAccount& from, const LXAccount& to,
                          const Currency& token, I128 amount_x18) {
    if (amount_x18 <= 0) {
        return errors::INVALID_PRICE;
    }

    uint64_t currency_hash = 0;
    for (auto b : token.addr) currency_hash = currency_hash * 31 + b;

    std::unique_lock lock(accounts_mutex_);

    AccountState* from_state = get_or_create_account(from);
    auto it = from_state->balances.find(currency_hash);
    if (it == from_state->balances.end() || it->second < amount_x18) {
        return errors::INSUFFICIENT_BALANCE;
    }

    from_state->balances[currency_hash] -= amount_x18;

    AccountState* to_state = get_or_create_account(to);
    to_state->balances[currency_hash] += amount_x18;

    return errors::OK;
}

I128 LXVault::get_balance(const LXAccount& account, const Currency& token) const {
    uint64_t currency_hash = 0;
    for (auto b : token.addr) currency_hash = currency_hash * 31 + b;

    std::shared_lock lock(accounts_mutex_);
    const AccountState* state = get_account(account);
    if (!state) return 0;

    auto it = state->balances.find(currency_hash);
    return (it != state->balances.end()) ? it->second : 0;
}

I128 LXVault::total_collateral_value(const LXAccount& account) const {
    std::shared_lock lock(accounts_mutex_);
    const AccountState* state = get_account(account);
    if (!state) return 0;

    I128 total = 0;
    for (const auto& [currency_hash, balance] : state->balances) {
        // Simplified: assume all tokens at 1:1 USD value
        // Production would use oracle for token prices
        total += balance;
    }
    return total;
}

// =============================================================================
// Margin Management
// =============================================================================

int32_t LXVault::set_margin_mode(const LXAccount& account, uint32_t market_id, MarginMode mode) {
    (void)market_id;  // Could be used for per-market margin mode in the future
    std::unique_lock lock(accounts_mutex_);
    AccountState* state = get_or_create_account(account);
    state->margin_mode = mode;
    return errors::OK;
}

std::optional<AccountState> LXVault::get_account_state(const LXAccount& account) const {
    std::shared_lock lock(accounts_mutex_);
    const AccountState* state = get_account(account);
    if (!state) return std::nullopt;
    return *state;
}

LXMarginInfo LXVault::get_margin_info(const LXAccount& account) const {
    LXMarginInfo info{};

    // Acquire both locks upfront in consistent order
    std::shared_lock accounts_lock(accounts_mutex_);
    std::shared_lock markets_lock(markets_mutex_);

    const AccountState* state = get_account(account);
    if (!state) return info;

    // Calculate total collateral
    info.total_collateral_x18 = 0;
    for (const auto& [currency_hash, balance] : state->balances) {
        info.total_collateral_x18 += balance;
    }

    // Calculate used margin and unrealized PnL
    I128 total_unrealized_pnl = 0;
    I128 total_initial_margin = 0;
    I128 total_maintenance_margin = 0;

    for (const auto& [market_id, position] : state->positions) {
        auto config_it = markets_.find(market_id);
        if (config_it == markets_.end()) continue;

        const MarketConfig& config = config_it->second;
        total_unrealized_pnl += position.unrealized_pnl_x18;
        total_initial_margin += calculate_initial_margin(position, config);
        total_maintenance_margin += calculate_maintenance_margin(position, config);
    }

    info.used_margin_x18 = total_initial_margin;
    info.maintenance_margin_x18 = total_maintenance_margin;

    I128 equity = info.total_collateral_x18 + total_unrealized_pnl;
    info.free_margin_x18 = equity - total_initial_margin;

    if (equity > 0) {
        info.margin_ratio_x18 = x18::div(total_maintenance_margin, equity);
    } else {
        info.margin_ratio_x18 = X18_ONE * 100; // 10000% if no equity
    }

    info.liquidatable = info.margin_ratio_x18 >= X18_ONE;

    return info;
}

I128 LXVault::account_equity_x18(const LXAccount& account) const {
    // Equity = collateral + unrealized PnL
    std::shared_lock lock(accounts_mutex_);
    const AccountState* state = get_account(account);
    if (!state) return 0;

    I128 total_collateral = 0;
    for (const auto& [currency_hash, balance] : state->balances) {
        total_collateral += balance;
    }

    I128 unrealized_pnl = 0;
    for (const auto& [market_id, position] : state->positions) {
        unrealized_pnl += position.unrealized_pnl_x18;
    }

    return total_collateral + unrealized_pnl;
}

I128 LXVault::margin_ratio_x18(const LXAccount& account) const {
    return get_margin_info(account).margin_ratio_x18;
}

int32_t LXVault::add_margin(const LXAccount& account, uint32_t market_id, I128 amount_x18) {
    // Stub: For isolated margin mode, move collateral from free balance to position margin
    (void)account;
    (void)market_id;
    (void)amount_x18;
    return errors::OK;
}

int32_t LXVault::remove_margin(const LXAccount& account, uint32_t market_id, I128 amount_x18) {
    // Stub: Check if position still meets margin requirements after removal
    (void)account;
    (void)market_id;
    (void)amount_x18;
    return errors::OK;
}

// =============================================================================
// Position Management
// =============================================================================

std::optional<LXPosition> LXVault::get_position(const LXAccount& account, uint32_t market_id) const {
    std::shared_lock lock(accounts_mutex_);
    const AccountState* state = get_account(account);
    if (!state) return std::nullopt;

    auto it = state->positions.find(market_id);
    if (it == state->positions.end()) return std::nullopt;
    return it->second;
}

std::vector<LXPosition> LXVault::get_all_positions(const LXAccount& account) const {
    std::vector<LXPosition> positions;

    std::shared_lock lock(accounts_mutex_);
    const AccountState* state = get_account(account);
    if (!state) return positions;

    for (const auto& [market_id, position] : state->positions) {
        positions.push_back(position);
    }

    return positions;
}

// =============================================================================
// Settlement
// =============================================================================

int32_t LXVault::pre_check_fills(const std::vector<LXSettlement>& settlements) {
    // Acquire locks in consistent order: accounts first, then markets
    std::shared_lock accounts_lock(accounts_mutex_);
    std::shared_lock markets_lock(markets_mutex_);

    for (const auto& settlement : settlements) {
        auto config_it = markets_.find(settlement.market_id);
        if (config_it == markets_.end()) {
            return errors::MARKET_NOT_FOUND;
        }

        if (!config_it->second.active) {
            return errors::MARKET_NOT_FOUND;
        }

        // Check if taker has margin for new position
        I128 notional = x18::mul(settlement.size_x18, settlement.price_x18);
        I128 required_margin = x18::mul(notional, config_it->second.initial_margin_x18);

        // Calculate taker's free margin inline
        const AccountState* taker_state = get_account(settlement.taker);
        if (taker_state) {
            I128 equity = 0;
            for (const auto& [hash, bal] : taker_state->balances) {
                equity += bal;
            }
            for (const auto& [mid, pos] : taker_state->positions) {
                equity += pos.unrealized_pnl_x18;
            }

            I128 used_margin = 0;
            for (const auto& [mid, pos] : taker_state->positions) {
                auto mit = markets_.find(mid);
                if (mit != markets_.end()) {
                    used_margin += calculate_initial_margin(pos, mit->second);
                }
            }

            I128 free_margin = equity - used_margin;
            if (free_margin < required_margin) {
                return errors::INSUFFICIENT_MARGIN;
            }
        }
    }

    return errors::OK;
}

int32_t LXVault::apply_fills(const std::vector<LXSettlement>& settlements) {
    std::unique_lock lock(accounts_mutex_);

    // FIX: Validate balances before fee deduction to prevent negative balances.
    // First pass: validate all fees can be paid
    uint64_t quote_hash = 0; // Simplified: use default currency
    for (const auto& settlement : settlements) {
        AccountState* maker_state = get_or_create_account(settlement.maker);
        AccountState* taker_state = get_or_create_account(settlement.taker);

        auto maker_it = maker_state->balances.find(quote_hash);
        I128 maker_bal = (maker_it != maker_state->balances.end()) ? maker_it->second : 0;
        if (maker_bal < settlement.maker_fee_x18) {
            return errors::INSUFFICIENT_BALANCE;
        }

        auto taker_it = taker_state->balances.find(quote_hash);
        I128 taker_bal = (taker_it != taker_state->balances.end()) ? taker_it->second : 0;
        if (taker_bal < settlement.taker_fee_x18) {
            return errors::INSUFFICIENT_BALANCE;
        }
    }

    // Second pass: apply all fills atomically
    for (const auto& settlement : settlements) {
        AccountState* maker_state = get_or_create_account(settlement.maker);
        AccountState* taker_state = get_or_create_account(settlement.taker);

        // Update maker position
        update_position(*maker_state, settlement.market_id,
                        !settlement.taker_is_buy, // Maker is opposite side
                        settlement.size_x18, settlement.price_x18);

        // Update taker position
        update_position(*taker_state, settlement.market_id,
                        settlement.taker_is_buy,
                        settlement.size_x18, settlement.price_x18);

        // Deduct fees (validated above)
        maker_state->balances[quote_hash] -= settlement.maker_fee_x18;
        taker_state->balances[quote_hash] -= settlement.taker_fee_x18;
    }

    return errors::OK;
}

// =============================================================================
// Liquidation
// =============================================================================

bool LXVault::is_liquidatable(const LXAccount& account) const {
    return get_margin_info(account).liquidatable;
}

LXLiquidationResult LXVault::liquidate(const LXAccount& liquidator, const LXAccount& account,
                                        uint32_t market_id, I128 size_x18) {
    LXLiquidationResult result{};
    result.liquidated = account;
    result.liquidator = liquidator;
    result.market_id = market_id;

    if (!is_liquidatable(account)) {
        return result;
    }

    std::unique_lock lock(accounts_mutex_);
    AccountState* state = get_or_create_account(account);

    auto pos_it = state->positions.find(market_id);
    if (pos_it == state->positions.end()) {
        return result;
    }

    LXPosition& position = pos_it->second;

    // Clamp liquidation size
    I128 liq_size = std::min(size_x18, position.size_x18 > 0 ? position.size_x18 : -position.size_x18);

    // FIX: Use mark price from callback, NOT entry price.
    // Entry price makes liquidation logic circular and broken.
    // mark_price_callback_ must be set via set_mark_price_callback().
    I128 mark_price = 0;
    if (mark_price_callback_) {
        mark_price = mark_price_callback_(market_id);
    }
    if (mark_price <= 0) {
        // Fallback: cannot liquidate without valid mark price
        return result;
    }

    result.size_x18 = liq_size;
    result.price_x18 = mark_price;

    // Calculate penalty (typically 0.5-1%)
    I128 notional = x18::mul(liq_size, mark_price);
    result.penalty_x18 = x18::mul(notional, x18::from_double(0.005)); // 0.5%

    // Update positions
    update_position(*state, market_id, position.side == PositionSide::LONG, -liq_size, mark_price);

    // Transfer penalty to insurance fund
    insurance_fund_.fetch_add(result.penalty_x18, std::memory_order_relaxed);

    total_liquidations_.fetch_add(1, std::memory_order_relaxed);

    return result;
}

int32_t LXVault::run_adl(uint32_t market_id) {
    // Stub: Auto-deleverage when insurance fund is depleted
    // Match profitable positions against underwater positions
    (void)market_id;
    return errors::OK;
}

// =============================================================================
// Funding
// =============================================================================

int32_t LXVault::accrue_funding(uint32_t market_id) {
    std::unique_lock funding_lock(funding_mutex_);

    auto it = funding_.find(market_id);
    if (it == funding_.end()) {
        return errors::MARKET_NOT_FOUND;
    }

    FundingState& funding = it->second;
    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    if (now < funding.last_funding_time + funding.funding_interval) {
        return errors::OK; // Not time yet
    }

    funding.cumulative_funding_x18 += funding.current_rate_x18;
    funding.last_funding_time = now;

    funding_lock.unlock();

    // Apply funding to all positions
    std::unique_lock accounts_lock(accounts_mutex_);

    for (auto& [account_hash, account_state] : accounts_) {
        auto pos_it = account_state.positions.find(market_id);
        if (pos_it == account_state.positions.end()) continue;

        LXPosition& position = pos_it->second;
        I128 funding_payment = x18::mul(position.size_x18, funding.current_rate_x18);

        // Long pays funding when rate is positive
        if (position.side == PositionSide::LONG) {
            position.accumulated_funding_x18 -= funding_payment;
        } else {
            position.accumulated_funding_x18 += funding_payment;
        }
        position.last_funding_time = now;
    }

    return errors::OK;
}

I128 LXVault::funding_rate_x18(uint32_t market_id) const {
    std::shared_lock lock(funding_mutex_);
    auto it = funding_.find(market_id);
    if (it == funding_.end()) return 0;
    return it->second.current_rate_x18;
}

uint64_t LXVault::next_funding_time(uint32_t market_id) const {
    std::shared_lock lock(funding_mutex_);
    auto it = funding_.find(market_id);
    if (it == funding_.end()) return 0;
    return it->second.last_funding_time + it->second.funding_interval;
}

void LXVault::set_funding_rate(uint32_t market_id, I128 rate_x18) {
    std::unique_lock lock(funding_mutex_);
    auto it = funding_.find(market_id);
    if (it != funding_.end()) {
        it->second.current_rate_x18 = rate_x18;
    }
}

// =============================================================================
// Insurance Fund
// =============================================================================

I128 LXVault::insurance_fund_balance() const {
    return insurance_fund_.load(std::memory_order_relaxed);
}

void LXVault::contribute_to_insurance(I128 amount_x18) {
    insurance_fund_.fetch_add(amount_x18, std::memory_order_relaxed);
}

I128 LXVault::withdraw_from_insurance(I128 amount_x18) {
    I128 current = insurance_fund_.load(std::memory_order_relaxed);
    I128 withdraw = std::min(amount_x18, current);

    while (!insurance_fund_.compare_exchange_weak(current, current - withdraw,
                                                   std::memory_order_relaxed)) {
        withdraw = std::min(amount_x18, current);
    }

    return withdraw;
}

// =============================================================================
// Mark-to-Market Updates
// =============================================================================

void LXVault::set_mark_price_callback(MarkPriceCallback callback) {
    mark_price_callback_ = std::move(callback);
}

int32_t LXVault::update_mark_prices(const std::vector<std::pair<uint32_t, I128>>& prices) {
    // FIX: Add mark-to-market updates for unrealized_pnl_x18.
    // Without this, unrealized_pnl_x18 is never updated and margin
    // calculations use stale values.
    std::unique_lock lock(accounts_mutex_);

    for (auto& [account_hash, account_state] : accounts_) {
        for (auto& [market_id, position] : account_state.positions) {
            // Find mark price for this market
            I128 mark_price = 0;
            for (const auto& [mid, price] : prices) {
                if (mid == market_id) {
                    mark_price = price;
                    break;
                }
            }
            if (mark_price <= 0) continue;

            // Update unrealized PnL
            position.unrealized_pnl_x18 = calculate_unrealized_pnl(position, mark_price);
        }
    }

    return errors::OK;
}

int32_t LXVault::update_position_mark(const LXAccount& account, uint32_t market_id, I128 mark_price_x18) {
    if (mark_price_x18 <= 0) {
        return errors::INVALID_PRICE;
    }

    std::unique_lock lock(accounts_mutex_);
    AccountState* state = get_or_create_account(account);

    auto pos_it = state->positions.find(market_id);
    if (pos_it == state->positions.end()) {
        return errors::POSITION_NOT_FOUND;
    }

    pos_it->second.unrealized_pnl_x18 = calculate_unrealized_pnl(pos_it->second, mark_price_x18);
    return errors::OK;
}

// =============================================================================
// Statistics
// =============================================================================

LXVault::Stats LXVault::get_stats() const {
    std::shared_lock lock(accounts_mutex_);

    uint64_t total_positions = 0;
    for (const auto& [account_hash, state] : accounts_) {
        total_positions += state.positions.size();
    }

    return Stats{
        accounts_.size(),
        total_positions,
        total_liquidations_.load(std::memory_order_relaxed),
        0, // total_volume_x18 - would need tracking
        0  // total_fees_collected_x18
    };
}

// =============================================================================
// Internal Helpers
// =============================================================================

AccountState* LXVault::get_or_create_account(const LXAccount& account) {
    uint64_t hash = account.hash();
    auto it = accounts_.find(hash);
    if (it == accounts_.end()) {
        AccountState state;
        state.margin_mode = MarginMode::CROSS;
        state.total_pnl_x18 = 0;
        state.last_update_time = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
        accounts_[hash] = std::move(state);
        it = accounts_.find(hash);
    }
    return &it->second;
}

const AccountState* LXVault::get_account(const LXAccount& account) const {
    uint64_t hash = account.hash();
    auto it = accounts_.find(hash);
    return (it != accounts_.end()) ? &it->second : nullptr;
}

I128 LXVault::calculate_initial_margin(const LXPosition& pos, const MarketConfig& config) const {
    I128 notional = x18::mul(pos.size_x18 > 0 ? pos.size_x18 : -pos.size_x18, pos.entry_px_x18);
    return x18::mul(notional, config.initial_margin_x18);
}

I128 LXVault::calculate_maintenance_margin(const LXPosition& pos, const MarketConfig& config) const {
    I128 notional = x18::mul(pos.size_x18 > 0 ? pos.size_x18 : -pos.size_x18, pos.entry_px_x18);
    return x18::mul(notional, config.maintenance_margin_x18);
}

I128 LXVault::calculate_unrealized_pnl(const LXPosition& pos, I128 mark_price_x18) const {
    I128 price_diff = mark_price_x18 - pos.entry_px_x18;
    if (pos.side == PositionSide::SHORT) {
        price_diff = -price_diff;
    }
    return x18::mul(pos.size_x18, price_diff);
}

void LXVault::update_position(AccountState& state, uint32_t market_id,
                               bool is_buy, I128 size_x18, I128 price_x18) {
    auto& position = state.positions[market_id];

    bool increasing = (is_buy && position.size_x18 >= 0) ||
                      (!is_buy && position.size_x18 <= 0);

    if (increasing || position.size_x18 == 0) {
        // Increasing position or opening new
        I128 old_notional = x18::mul(position.size_x18 > 0 ? position.size_x18 : -position.size_x18,
                                      position.entry_px_x18);
        I128 new_notional = x18::mul(size_x18, price_x18);
        I128 total_size = position.size_x18 + (is_buy ? size_x18 : -size_x18);
        I128 abs_total = total_size > 0 ? total_size : -total_size;

        if (abs_total > 0) {
            position.entry_px_x18 = x18::div(old_notional + new_notional, abs_total);
        }
        position.size_x18 = total_size;
        position.side = total_size >= 0 ? PositionSide::LONG : PositionSide::SHORT;
    } else {
        // Reducing position
        I128 reduction = is_buy ? size_x18 : -size_x18;
        I128 old_size = position.size_x18 > 0 ? position.size_x18 : -position.size_x18;
        I128 reduce_abs = reduction > 0 ? reduction : -reduction;
        reduce_abs = std::min(reduce_abs, old_size);

        // Realize PnL
        I128 pnl = calculate_unrealized_pnl(position, price_x18);
        pnl = x18::mul(pnl, x18::div(reduce_abs, old_size));
        state.total_pnl_x18 += pnl;

        position.size_x18 += reduction;
        if (position.size_x18 == 0) {
            close_position(state, market_id);
        } else {
            position.side = position.size_x18 >= 0 ? PositionSide::LONG : PositionSide::SHORT;
        }
    }

    position.market_id = market_id;
    position.last_funding_time = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

void LXVault::close_position(AccountState& state, uint32_t market_id) {
    state.positions.erase(market_id);
}

I128 LXVault::calculate_fee(I128 notional_x18, I128 fee_rate_x18) const {
    return x18::mul(notional_x18, fee_rate_x18);
}

// =============================================================================
// RiskEngine Implementation
// =============================================================================

RiskEngine::RiskEngine(LXVault& vault) : vault_(vault) {}

I128 RiskEngine::calculate_portfolio_margin(const LXAccount& account) const {
    // Portfolio margin with cross-margining
    return vault_.get_margin_info(account).used_margin_x18;
}

bool RiskEngine::pre_trade_check(const LXAccount& account, const LXOrder& order) const {
    LXMarginInfo margin = vault_.get_margin_info(account);

    // Estimate margin required for this order
    I128 notional = x18::mul(order.size_x18, order.limit_px_x18);

    auto config = vault_.get_market_config(order.market_id);
    if (!config) return false;

    I128 required = x18::mul(notional, config->initial_margin_x18);
    return margin.free_margin_x18 >= required;
}

bool RiskEngine::is_bankrupt(const LXAccount& account) const {
    return vault_.account_equity_x18(account) <= 0;
}

I128 RiskEngine::max_order_size(const LXAccount& account, uint32_t market_id, bool is_buy) const {
    (void)is_buy;  // Could be used for position-aware sizing in the future
    LXMarginInfo margin = vault_.get_margin_info(account);
    if (margin.free_margin_x18 <= 0) return 0;

    auto config = vault_.get_market_config(market_id);
    if (!config) return 0;

    // max_size = free_margin / (price * initial_margin_rate)
    // Simplified: assume price = 1
    return x18::div(margin.free_margin_x18, config->initial_margin_x18);
}

I128 RiskEngine::liquidation_price(const LXAccount& account, uint32_t market_id) const {
    auto position = vault_.get_position(account, market_id);
    if (!position) return 0;

    auto config = vault_.get_market_config(market_id);
    if (!config) return 0;

    I128 equity = vault_.account_equity_x18(account);
    I128 size_abs = position->size_x18 > 0 ? position->size_x18 : -position->size_x18;

    if (size_abs == 0) return 0;

    // liq_price = entry - (equity - mm * notional) / size for longs
    // liq_price = entry + (equity - mm * notional) / size for shorts
    I128 notional = x18::mul(size_abs, position->entry_px_x18);
    I128 mm = x18::mul(notional, config->maintenance_margin_x18);
    I128 buffer = equity - mm;
    I128 price_buffer = x18::div(buffer, size_abs);

    if (position->side == PositionSide::LONG) {
        return position->entry_px_x18 - price_buffer;
    } else {
        return position->entry_px_x18 + price_buffer;
    }
}

} // namespace lux
