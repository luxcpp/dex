#ifndef LUX_VAULT_HPP
#define LUX_VAULT_HPP

#include <map>
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include <vector>
#include <atomic>
#include <functional>

#include "types.hpp"

namespace lux {

// =============================================================================
// Market Configuration
// =============================================================================

struct MarketConfig {
    uint32_t market_id;
    Currency base_currency;
    Currency quote_currency;
    I128 initial_margin_x18;      // e.g., 0.1 = 10%
    I128 maintenance_margin_x18;  // e.g., 0.05 = 5%
    I128 max_leverage_x18;        // e.g., 20 = 20x
    I128 taker_fee_x18;           // e.g., 0.0005 = 0.05%
    I128 maker_fee_x18;           // e.g., 0.0002 = 0.02%
    I128 min_order_size_x18;
    I128 max_position_size_x18;
    bool reduce_only_mode;        // Deleveraging mode
    bool active;
};

// =============================================================================
// Account State
// =============================================================================

struct AccountState {
    MarginMode margin_mode;
    std::unordered_map<uint64_t, I128> balances;     // currency_hash -> balance_x18
    std::map<uint32_t, LXPosition> positions;        // market_id -> position
    I128 total_pnl_x18;
    uint64_t last_update_time;
};

// =============================================================================
// Settlement Record
// =============================================================================

struct LXSettlement {
    LXAccount maker;
    LXAccount taker;
    uint32_t market_id;
    bool taker_is_buy;
    I128 size_x18;
    I128 price_x18;
    I128 maker_fee_x18;
    I128 taker_fee_x18;
    uint8_t flags;  // fill_flags
};

// =============================================================================
// Liquidation Result
// =============================================================================

struct LXLiquidationResult {
    LXAccount liquidated;
    LXAccount liquidator;
    uint32_t market_id;
    I128 size_x18;
    I128 price_x18;
    I128 penalty_x18;
    bool adl_triggered;
};

// =============================================================================
// LXVault - Clearinghouse with Custody, Margin, Positions, Liquidations
// =============================================================================

class LXVault {
public:
    LXVault();
    ~LXVault() = default;

    // Non-copyable
    LXVault(const LXVault&) = delete;
    LXVault& operator=(const LXVault&) = delete;

    // =========================================================================
    // Market Management
    // =========================================================================

    int32_t create_market(const MarketConfig& config);
    int32_t update_market(const MarketConfig& config);
    std::optional<MarketConfig> get_market_config(uint32_t market_id) const;
    bool market_exists(uint32_t market_id) const;

    // =========================================================================
    // Deposit/Withdraw (Custody)
    // =========================================================================

    // Deposit collateral
    int32_t deposit(const LXAccount& account, const Currency& token, I128 amount_x18);

    // Withdraw collateral (checks margin requirements)
    int32_t withdraw(const LXAccount& account, const Currency& token, I128 amount_x18);

    // Internal transfer between subaccounts
    int32_t transfer(const LXAccount& from, const LXAccount& to,
                     const Currency& token, I128 amount_x18);

    // Get balance
    I128 get_balance(const LXAccount& account, const Currency& token) const;
    I128 total_collateral_value(const LXAccount& account) const;

    // =========================================================================
    // Margin Management
    // =========================================================================

    // Set margin mode (cross/isolated)
    int32_t set_margin_mode(const LXAccount& account, uint32_t market_id, MarginMode mode);

    // Get account state
    std::optional<AccountState> get_account_state(const LXAccount& account) const;

    // Get margin info
    LXMarginInfo get_margin_info(const LXAccount& account) const;

    // Account equity (collateral + unrealized PnL)
    I128 account_equity_x18(const LXAccount& account) const;

    // Margin ratio = used_margin / equity
    I128 margin_ratio_x18(const LXAccount& account) const;

    // Add/remove margin for isolated position
    int32_t add_margin(const LXAccount& account, uint32_t market_id, I128 amount_x18);
    int32_t remove_margin(const LXAccount& account, uint32_t market_id, I128 amount_x18);

    // =========================================================================
    // Position Management
    // =========================================================================

    // Get position
    std::optional<LXPosition> get_position(const LXAccount& account, uint32_t market_id) const;

    // Get all positions
    std::vector<LXPosition> get_all_positions(const LXAccount& account) const;

    // =========================================================================
    // Settlement (from CLOB matches)
    // =========================================================================

    // Pre-check fills (validate margin requirements)
    int32_t pre_check_fills(const std::vector<LXSettlement>& settlements);

    // Apply fills (update positions after matching)
    int32_t apply_fills(const std::vector<LXSettlement>& settlements);

    // =========================================================================
    // Liquidation
    // =========================================================================

    // Check if account is liquidatable
    bool is_liquidatable(const LXAccount& account) const;

    // Liquidate a position
    LXLiquidationResult liquidate(const LXAccount& liquidator, const LXAccount& account,
                                   uint32_t market_id, I128 size_x18);

    // Auto-deleverage (socialized losses)
    int32_t run_adl(uint32_t market_id);

    // =========================================================================
    // Funding
    // =========================================================================

    // Accrue funding for all positions in a market
    int32_t accrue_funding(uint32_t market_id);

    // Get current funding rate
    I128 funding_rate_x18(uint32_t market_id) const;

    // Get next funding time
    uint64_t next_funding_time(uint32_t market_id) const;

    // Set funding rate (called by LXFeed)
    void set_funding_rate(uint32_t market_id, I128 rate_x18);

    // =========================================================================
    // Insurance Fund
    // =========================================================================

    I128 insurance_fund_balance() const;
    void contribute_to_insurance(I128 amount_x18);
    I128 withdraw_from_insurance(I128 amount_x18);

    // =========================================================================
    // Mark-to-Market Updates
    // =========================================================================

    using MarkPriceCallback = std::function<I128(uint32_t)>;  // market_id -> mark_price

    // Set callback to fetch mark prices
    void set_mark_price_callback(MarkPriceCallback callback);

    // Update mark prices for all positions
    int32_t update_mark_prices(const std::vector<std::pair<uint32_t, I128>>& prices);

    // Update mark price for single position
    int32_t update_position_mark(const LXAccount& account, uint32_t market_id, I128 mark_price_x18);

    // =========================================================================
    // Statistics
    // =========================================================================

    struct Stats {
        uint64_t total_accounts;
        uint64_t total_positions;
        uint64_t total_liquidations;
        I128 total_volume_x18;
        I128 total_fees_collected_x18;
    };
    Stats get_stats() const;

private:
    // Account storage: account_hash -> state
    std::unordered_map<uint64_t, AccountState> accounts_;
    mutable std::shared_mutex accounts_mutex_;

    // Market configs
    std::unordered_map<uint32_t, MarketConfig> markets_;
    mutable std::shared_mutex markets_mutex_;

    // Funding state per market
    struct FundingState {
        I128 current_rate_x18;
        I128 cumulative_funding_x18;
        uint64_t last_funding_time;
        uint64_t funding_interval;  // seconds
    };
    std::unordered_map<uint32_t, FundingState> funding_;
    mutable std::shared_mutex funding_mutex_;

    // Insurance fund
    std::atomic<I128> insurance_fund_{0};

    // Statistics
    std::atomic<uint64_t> total_liquidations_{0};

    // Mark price callback
    MarkPriceCallback mark_price_callback_;

    // Internal helpers
    AccountState* get_or_create_account(const LXAccount& account);
    const AccountState* get_account(const LXAccount& account) const;

    // Margin calculations
    I128 calculate_initial_margin(const LXPosition& pos, const MarketConfig& config) const;
    I128 calculate_maintenance_margin(const LXPosition& pos, const MarketConfig& config) const;
    I128 calculate_unrealized_pnl(const LXPosition& pos, I128 mark_price_x18) const;

    // Position updates
    void update_position(AccountState& state, uint32_t market_id,
                         bool is_buy, I128 size_x18, I128 price_x18);
    void close_position(AccountState& state, uint32_t market_id);

    // Fee calculation
    I128 calculate_fee(I128 notional_x18, I128 fee_rate_x18) const;
};

// =============================================================================
// Risk Engine
// =============================================================================

class RiskEngine {
public:
    RiskEngine(LXVault& vault);

    // Portfolio-level margin calculation
    I128 calculate_portfolio_margin(const LXAccount& account) const;

    // Check order pre-trade
    bool pre_trade_check(const LXAccount& account, const LXOrder& order) const;

    // Bankruptcy check
    bool is_bankrupt(const LXAccount& account) const;

    // Maximum order size given current margin
    I128 max_order_size(const LXAccount& account, uint32_t market_id, bool is_buy) const;

    // Liquidation price for a position
    I128 liquidation_price(const LXAccount& account, uint32_t market_id) const;

private:
    LXVault& vault_;
};

} // namespace lux

#endif // LUX_VAULT_HPP
