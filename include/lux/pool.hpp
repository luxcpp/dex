#ifndef LUX_POOL_HPP
#define LUX_POOL_HPP

#include <map>
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include <vector>
#include <functional>
#include <cmath>

#include "types.hpp"

namespace lux {

// =============================================================================
// Pool Slot0 State
// =============================================================================

struct Slot0 {
    I128 sqrt_price_x96;    // Current sqrt(price) as Q64.96
    int32_t tick;           // Current tick
    uint32_t protocol_fee;  // Protocol fee (hundredths of bip)
    uint32_t lp_fee;        // LP fee (hundredths of bip)
    bool unlocked;          // Reentrancy lock
};

// =============================================================================
// Tick Info
// =============================================================================

struct TickInfo {
    I128 liquidity_gross;        // Total liquidity at tick
    I128 liquidity_net;          // Net liquidity change when crossing
    I128 fee_growth_outside0_x128;
    I128 fee_growth_outside1_x128;
    bool initialized;
};

// =============================================================================
// Position Info
// =============================================================================

struct PositionInfo {
    I128 liquidity;              // Position liquidity
    I128 fee_growth_inside0_last_x128;
    I128 fee_growth_inside1_last_x128;
    I128 tokens_owed0;           // Uncollected fees
    I128 tokens_owed1;
};

// =============================================================================
// Pool State (single pool)
// =============================================================================

struct PoolState {
    Slot0 slot0;
    I128 fee_growth_global0_x128;
    I128 fee_growth_global1_x128;
    I128 protocol_fees0;
    I128 protocol_fees1;
    I128 liquidity;              // Current active liquidity
    std::map<int32_t, TickInfo> ticks;
    std::unordered_map<uint64_t, PositionInfo> positions;  // position_key -> info
};

// =============================================================================
// Flash Context (explicit accounting state for lock operations)
// =============================================================================

struct FlashContext {
    std::unordered_map<uint64_t, I128> currency_deltas;
    bool locked = false;

    void reset() {
        currency_deltas.clear();
        locked = false;
    }
};

// =============================================================================
// Hook Interface
// =============================================================================

class IHooks {
public:
    virtual ~IHooks() = default;

    // Called before/after each operation
    virtual bool before_initialize(const PoolKey& key, I128 sqrt_price_x96) { return true; }
    virtual void after_initialize(const PoolKey& key, I128 sqrt_price_x96, int32_t tick) {}

    virtual bool before_swap(const PoolKey& key, const SwapParams& params) { return true; }
    virtual void after_swap(const PoolKey& key, const SwapParams& params, const BalanceDelta& delta) {}

    virtual bool before_modify_liquidity(const PoolKey& key, const ModifyLiquidityParams& params) { return true; }
    virtual void after_modify_liquidity(const PoolKey& key, const ModifyLiquidityParams& params, const BalanceDelta& delta) {}

    virtual bool before_donate(const PoolKey& key, I128 amount0, I128 amount1) { return true; }
    virtual void after_donate(const PoolKey& key, I128 amount0, I128 amount1) {}
};

// Null hooks (no-op)
class NullHooks : public IHooks {};

// =============================================================================
// LXPool - Uniswap v4-style AMM Pool Manager
// =============================================================================

class LXPool {
public:
    LXPool();
    ~LXPool() = default;

    // Non-copyable
    LXPool(const LXPool&) = delete;
    LXPool& operator=(const LXPool&) = delete;

    // =========================================================================
    // Core Operations
    // =========================================================================

    // Initialize a new pool
    // Returns: initial tick, or error code
    int32_t initialize(const PoolKey& key, I128 sqrt_price_x96);

    // Swap tokens
    // Returns: balance delta (amount0, amount1), positive = tokens owed to pool
    BalanceDelta swap(const PoolKey& key, const SwapParams& params,
                      const std::vector<uint8_t>& hook_data = {});

    // Add or remove liquidity
    // Returns: balance delta for principal + fees
    BalanceDelta modify_liquidity(const PoolKey& key, const ModifyLiquidityParams& params,
                                  const std::vector<uint8_t>& hook_data = {});

    // Donate tokens to in-range liquidity providers
    BalanceDelta donate(const PoolKey& key, I128 amount0, I128 amount1,
                        const std::vector<uint8_t>& hook_data = {});

    // =========================================================================
    // Flash Accounting (Uniswap v4 transient storage pattern)
    // =========================================================================

    // Lock the pool manager for a flash operation
    // Uses member variables for delta tracking (simpler than FlashContext parameter)
    using LockCallback = std::function<void()>;
    void lock(LockCallback callback);

    // Take tokens out (creates debt) - must be called within lock()
    void take(const Currency& currency, const Address& to, I128 amount);

    // Settle debt (pay tokens in) - must be called within lock()
    I128 settle(const Currency& currency);

    // Sync after external transfer - must be called within lock()
    void sync(const Currency& currency);

    // Operations with explicit flash context (for composability)
    BalanceDelta swap(FlashContext& ctx, const PoolKey& key, const SwapParams& params,
                      const std::vector<uint8_t>& hook_data = {});
    BalanceDelta modify_liquidity(FlashContext& ctx, const PoolKey& key, const ModifyLiquidityParams& params,
                                  const std::vector<uint8_t>& hook_data = {});
    BalanceDelta donate(FlashContext& ctx, const PoolKey& key, I128 amount0, I128 amount1,
                        const std::vector<uint8_t>& hook_data = {});

    // =========================================================================
    // Query Operations
    // =========================================================================

    // Get pool state
    std::optional<Slot0> get_slot0(const PoolKey& key) const;
    std::optional<I128> get_liquidity(const PoolKey& key) const;
    std::optional<PositionInfo> get_position(const PoolKey& key,
                                              const Address& owner,
                                              int32_t tick_lower,
                                              int32_t tick_upper,
                                              uint64_t salt = 0) const;

    // Check if pool exists
    bool pool_exists(const PoolKey& key) const;

    // =========================================================================
    // Protocol Fee Management
    // =========================================================================

    void set_protocol_fee(const PoolKey& key, uint32_t new_fee);
    BalanceDelta collect_protocol(const PoolKey& key, const Address& recipient);

    // =========================================================================
    // Hook Registration
    // =========================================================================

    void register_hooks(const Address& hook_addr, IHooks* hooks);
    void unregister_hooks(const Address& hook_addr);

    // =========================================================================
    // Statistics
    // =========================================================================

    struct Stats {
        uint64_t total_pools;
        uint64_t total_swaps;
        uint64_t total_liquidity_ops;
        I128 total_volume0_x18;
        I128 total_volume1_x18;
    };
    Stats get_stats() const;

private:
    // Pool storage: pool_id -> state
    std::unordered_map<uint64_t, PoolState> pools_;
    mutable std::shared_mutex pools_mutex_;

    // Hook registry
    std::unordered_map<uint64_t, IHooks*> hooks_;  // hash(address) -> hooks
    mutable std::shared_mutex hooks_mutex_;

    // Flash accounting state
    bool locked_{false};
    std::unordered_map<uint64_t, I128> currency_deltas_;  // currency_hash -> delta

    // Statistics
    std::atomic<uint64_t> total_swaps_{0};
    std::atomic<uint64_t> total_liquidity_ops_{0};

    // Internal helpers
    PoolState* get_pool(const PoolKey& key);
    const PoolState* get_pool(const PoolKey& key) const;
    IHooks* get_hooks(const PoolKey& key);

    // Tick math
    static int32_t get_tick_at_sqrt_ratio(I128 sqrt_price_x96);
    static I128 get_sqrt_ratio_at_tick(int32_t tick);

    // Position key
    static uint64_t position_key(const Address& owner, int32_t tick_lower,
                                  int32_t tick_upper, uint64_t salt);

    // Swap computation
    struct SwapState {
        I128 amount_remaining;
        I128 amount_calculated;
        I128 sqrt_price_x96;
        int32_t tick;
        I128 liquidity;
    };
    SwapState compute_swap_step(SwapState state, I128 sqrt_price_target_x96,
                                 uint32_t fee_pips, bool zero_for_one);
};

// =============================================================================
// Tick Math Utilities
// =============================================================================

namespace tick_math {

// Minimum and maximum ticks
constexpr int32_t MIN_TICK = -887272;
constexpr int32_t MAX_TICK = 887272;

// Minimum and maximum sqrt ratios (Q64.96)
// These are: sqrt(1.0001^MIN_TICK) and sqrt(1.0001^MAX_TICK) * 2^96
const I128 MIN_SQRT_RATIO = 4295128739LL;
// MAX_SQRT_RATIO: sqrt(1.0001^887272) * 2^96
// Computed dynamically to avoid literal overflow in C++
inline I128 max_sqrt_ratio() {
    // Value: 1461446703485210103287273052203988822378723970342
    // Split into parts that fit in 64-bit: high * 2^128 would overflow
    // Use smaller decomposition: compute from tick at runtime
    double val = std::pow(1.0001, MAX_TICK / 2.0);
    double scaled = val * static_cast<double>(1ULL << 48) * static_cast<double>(1ULL << 48);
    return static_cast<I128>(scaled);
}
const I128 MAX_SQRT_RATIO = max_sqrt_ratio();

// Get tick at sqrt ratio
inline int32_t get_tick_at_sqrt_ratio(I128 sqrt_price_x96) {
    // Simplified: log_1.0001(price) where price = (sqrt_price / 2^96)^2
    // tick = log(price) / log(1.0001)
    if (sqrt_price_x96 <= MIN_SQRT_RATIO) return MIN_TICK;
    if (sqrt_price_x96 >= MAX_SQRT_RATIO) return MAX_TICK;

    // Convert to double for computation (production would use fixed-point)
    double sqrt_price = static_cast<double>(sqrt_price_x96) / static_cast<double>(1ULL << 48);
    sqrt_price /= static_cast<double>(1ULL << 48);  // Total: 2^96
    double price = sqrt_price * sqrt_price;
    double tick_d = std::log(price) / std::log(1.0001);
    return static_cast<int32_t>(std::floor(tick_d));
}

// Get sqrt ratio at tick
inline I128 get_sqrt_ratio_at_tick(int32_t tick) {
    if (tick < MIN_TICK || tick > MAX_TICK) return 0;

    // sqrt_price = sqrt(1.0001^tick)
    double sqrt_price = std::pow(1.0001, tick / 2.0);
    // Convert to Q64.96
    double scaled = sqrt_price * static_cast<double>(1ULL << 48) * static_cast<double>(1ULL << 48);
    return static_cast<I128>(scaled);
}

} // namespace tick_math

// =============================================================================
// Liquidity Math Utilities
// =============================================================================

namespace liquidity_math {

// Calculate liquidity from token amounts
inline I128 get_liquidity_for_amounts(
    I128 sqrt_price_x96,
    I128 sqrt_price_a_x96,
    I128 sqrt_price_b_x96,
    I128 amount0,
    I128 amount1
) {
    if (sqrt_price_a_x96 > sqrt_price_b_x96) {
        std::swap(sqrt_price_a_x96, sqrt_price_b_x96);
    }

    if (sqrt_price_x96 <= sqrt_price_a_x96) {
        // Below range: all token0
        return x18::div(amount0 * (sqrt_price_b_x96 - sqrt_price_a_x96),
                       sqrt_price_b_x96 * sqrt_price_a_x96);
    } else if (sqrt_price_x96 < sqrt_price_b_x96) {
        // In range: use both
        I128 liquidity0 = x18::div(amount0 * (sqrt_price_b_x96 - sqrt_price_x96),
                                   sqrt_price_b_x96 * sqrt_price_x96);
        I128 liquidity1 = x18::div(amount1, sqrt_price_x96 - sqrt_price_a_x96);
        return liquidity0 < liquidity1 ? liquidity0 : liquidity1;
    } else {
        // Above range: all token1
        return x18::div(amount1, sqrt_price_b_x96 - sqrt_price_a_x96);
    }
}

// Calculate token amounts from liquidity
inline std::pair<I128, I128> get_amounts_for_liquidity(
    I128 sqrt_price_x96,
    I128 sqrt_price_a_x96,
    I128 sqrt_price_b_x96,
    I128 liquidity
) {
    if (sqrt_price_a_x96 > sqrt_price_b_x96) {
        std::swap(sqrt_price_a_x96, sqrt_price_b_x96);
    }

    I128 amount0 = 0, amount1 = 0;

    if (sqrt_price_x96 <= sqrt_price_a_x96) {
        // Below range
        amount0 = x18::mul(liquidity, sqrt_price_b_x96 - sqrt_price_a_x96) *
                  sqrt_price_a_x96 / sqrt_price_b_x96;
    } else if (sqrt_price_x96 < sqrt_price_b_x96) {
        // In range
        amount0 = x18::mul(liquidity, sqrt_price_b_x96 - sqrt_price_x96) *
                  sqrt_price_x96 / sqrt_price_b_x96;
        amount1 = x18::mul(liquidity, sqrt_price_x96 - sqrt_price_a_x96);
    } else {
        // Above range
        amount1 = x18::mul(liquidity, sqrt_price_b_x96 - sqrt_price_a_x96);
    }

    return {amount0, amount1};
}

} // namespace liquidity_math

} // namespace lux

#endif // LUX_POOL_HPP
