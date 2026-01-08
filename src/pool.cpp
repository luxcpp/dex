// =============================================================================
// pool.cpp - LXPool AMM Implementation (Uniswap v4-style)
// Production C++17 with flash accounting and concentrated liquidity
// =============================================================================

#include "lux/pool.hpp"
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace lux {


// =============================================================================
// Internal Constants
// =============================================================================

namespace {

// Q64.96 scaling factor
constexpr I128 Q96 = I128(1) << 96;
// Q128: 2^128 - computed as two 64-bit halves to avoid overflow
inline I128 make_q128() {
    I128 high = I128(1) << 64;
    return high << 64;
}
const I128 Q128 = make_q128();

// Fee denominator: 1e6 (1 pip = 0.0001%)
constexpr uint32_t FEE_DENOMINATOR = 1000000;

// Hash currency address for delta tracking
inline uint64_t currency_hash(const Currency& c) {
    uint64_t h = 0;
    for (uint8_t b : c.addr) h = h * 31 + b;
    return h;
}

// Hash address for hook lookup
inline uint64_t address_hash(const Address& a) {
    uint64_t h = 0;
    for (uint8_t b : a) h = h * 31 + b;
    return h;
}

// Check if address is zero
inline bool is_zero_address(const Address& a) {
    for (uint8_t b : a) if (b != 0) return false;
    return true;
}

// Absolute value
inline I128 abs128(I128 x) { return x < 0 ? -x : x; }

// =============================================================================
// 256-bit Arithmetic (U256 via two U128 limbs)
// =============================================================================

struct U256 {
    U128 lo;  // Low 128 bits
    U128 hi;  // High 128 bits

    U256() : lo(0), hi(0) {}
    U256(U128 l) : lo(l), hi(0) {}
    U256(U128 l, U128 h) : lo(l), hi(h) {}

    bool operator==(const U256& other) const {
        return lo == other.lo && hi == other.hi;
    }
    bool operator<(const U256& other) const {
        return hi < other.hi || (hi == other.hi && lo < other.lo);
    }
    bool operator>=(const U256& other) const {
        return !(*this < other);
    }
    bool is_zero() const { return lo == 0 && hi == 0; }
};

// Multiply two U128 values to produce U256
inline U256 mul_u128(U128 a, U128 b) {
    // Split into 64-bit halves to avoid overflow
    constexpr U128 MASK64 = (U128(1) << 64) - 1;
    U128 a_lo = a & MASK64;
    U128 a_hi = a >> 64;
    U128 b_lo = b & MASK64;
    U128 b_hi = b >> 64;

    // Cross products
    U128 p0 = a_lo * b_lo;           // lo * lo
    U128 p1 = a_lo * b_hi;           // lo * hi
    U128 p2 = a_hi * b_lo;           // hi * lo
    U128 p3 = a_hi * b_hi;           // hi * hi

    // Accumulate with carry
    U128 mid = (p0 >> 64) + (p1 & MASK64) + (p2 & MASK64);
    U128 carry = mid >> 64;

    U256 result;
    result.lo = (p0 & MASK64) | (mid << 64);
    result.hi = p3 + (p1 >> 64) + (p2 >> 64) + carry;
    return result;
}

// Divide U256 by U128, return U128 quotient (assumes quotient fits)
inline U128 div_u256_u128(U256 num, U128 denom) {
    if (denom == 0) return 0;
    if (num.hi == 0) return num.lo / denom;

    // Binary long division for 256/128
    U256 rem = num;
    U128 quot = 0;

    // Find highest bit position in numerator
    int shift = 0;
    if (rem.hi != 0) {
        // Count leading zeros in hi
        U128 tmp = rem.hi;
        while (tmp != 0) { tmp >>= 1; shift++; }
        shift += 128;
    } else {
        U128 tmp = rem.lo;
        while (tmp != 0) { tmp >>= 1; shift++; }
    }

    // Long division bit by bit
    for (int i = shift - 1; i >= 0; --i) {
        // Shift quotient left
        quot <<= 1;

        // Get bit i of remainder
        U128 bit;
        if (i >= 128) {
            bit = (rem.hi >> (i - 128)) & 1;
        } else {
            bit = (rem.lo >> i) & 1;
        }

        // Build current dividend portion (accumulate bits)
        // Check if current portion >= denom
        U256 test;
        if (i >= 128) {
            test.hi = rem.hi >> (i - 128);
            test.lo = 0;
        } else if (i >= 0) {
            test.hi = 0;
            test.lo = (rem.hi << (128 - i)) | (rem.lo >> i);
        }

        if (test.hi > 0 || test.lo >= denom) {
            quot |= 1;
            // Subtract denom << i from rem
            U256 sub;
            if (i >= 128) {
                sub.hi = denom << (i - 128);
                sub.lo = 0;
            } else {
                sub.lo = denom << i;
                sub.hi = (i > 0) ? (denom >> (128 - i)) : 0;
            }
            // rem -= sub
            if (rem.lo < sub.lo) {
                rem.lo = rem.lo - sub.lo;  // Wraps
                rem.hi -= 1;               // Borrow
            } else {
                rem.lo -= sub.lo;
            }
            rem.hi -= sub.hi;
        }
    }
    return quot;
}

// Check if U256 is exactly divisible by U128
inline bool u256_mod_nonzero(U256 num, U128 denom) {
    if (denom == 0) return false;
    if (num.hi == 0) return (num.lo % denom) != 0;

    // Compute remainder using the quotient
    U128 quot = div_u256_u128(num, denom);
    U256 prod = mul_u128(quot, denom);
    return !(prod == num);
}

// =============================================================================
// Safe mul_div with 256-bit intermediate
// =============================================================================

inline I128 mul_div(I128 a, I128 b, I128 denom) {
    if (denom == 0) return 0;

    bool neg = (a < 0) ^ (b < 0) ^ (denom < 0);
    U128 ua = static_cast<U128>(abs128(a));
    U128 ub = static_cast<U128>(abs128(b));
    U128 ud = static_cast<U128>(abs128(denom));

    // 256-bit product, then divide
    U256 product = mul_u128(ua, ub);
    U128 result = div_u256_u128(product, ud);

    return neg ? -static_cast<I128>(result) : static_cast<I128>(result);
}

// Mul-div rounding up
inline I128 mul_div_up(I128 a, I128 b, I128 denom) {
    if (denom == 0) return 0;

    bool neg = (a < 0) ^ (b < 0) ^ (denom < 0);
    U128 ua = static_cast<U128>(abs128(a));
    U128 ub = static_cast<U128>(abs128(b));
    U128 ud = static_cast<U128>(abs128(denom));

    U256 product = mul_u128(ua, ub);
    U128 result = div_u256_u128(product, ud);

    // Round up if there's a remainder and result is positive
    if (!neg && u256_mod_nonzero(product, ud)) {
        result += 1;
    }

    return neg ? -static_cast<I128>(result) : static_cast<I128>(result);
}

} // anonymous namespace

// =============================================================================
// Constructor
// =============================================================================

LXPool::LXPool() = default;

// =============================================================================
// Internal Helpers
// =============================================================================

PoolState* LXPool::get_pool(const PoolKey& key) {
    auto it = pools_.find(key.id());
    return it != pools_.end() ? &it->second : nullptr;
}

const PoolState* LXPool::get_pool(const PoolKey& key) const {
    auto it = pools_.find(key.id());
    return it != pools_.end() ? &it->second : nullptr;
}

IHooks* LXPool::get_hooks(const PoolKey& key) {
    if (is_zero_address(key.hooks)) return nullptr;
    std::shared_lock lock(hooks_mutex_);
    auto it = hooks_.find(address_hash(key.hooks));
    return it != hooks_.end() ? it->second : nullptr;
}

int32_t LXPool::get_tick_at_sqrt_ratio(I128 sqrt_price_x96) {
    return tick_math::get_tick_at_sqrt_ratio(sqrt_price_x96);
}

I128 LXPool::get_sqrt_ratio_at_tick(int32_t tick) {
    return tick_math::get_sqrt_ratio_at_tick(tick);
}

uint64_t LXPool::position_key(const Address& owner, int32_t tick_lower,
                               int32_t tick_upper, uint64_t salt) {
    uint64_t h = salt;
    for (uint8_t b : owner) h = h * 31 + b;
    // Offset ticks to ensure positive values for hashing
    h = h * 31 + static_cast<uint64_t>(static_cast<uint32_t>(tick_lower + 1000000));
    h = h * 31 + static_cast<uint64_t>(static_cast<uint32_t>(tick_upper + 1000000));
    return h;
}

// =============================================================================
// Initialize Pool
// =============================================================================

int32_t LXPool::initialize(const PoolKey& key, I128 sqrt_price_x96) {
    // Validate: currencies must be sorted
    if (!(key.currency0 < key.currency1)) {
        return errors::CURRENCIES_NOT_SORTED;
    }

    // Validate: sqrt price in valid range
    if (sqrt_price_x96 < tick_math::MIN_SQRT_RATIO ||
        sqrt_price_x96 >= tick_math::MAX_SQRT_RATIO) {
        return errors::INVALID_PRICE;
    }

    // Validate: fee not exceeding maximum
    if (key.fee > fees::FEE_MAX) {
        return errors::INVALID_FEE;
    }

    // Validate: tick spacing positive
    if (key.tick_spacing <= 0) {
        return errors::INVALID_TICK_RANGE;
    }

    // Call before_initialize hook
    IHooks* hooks = get_hooks(key);
    if (hooks && !hooks->before_initialize(key, sqrt_price_x96)) {
        return errors::HOOK_FAILED;
    }

    std::unique_lock lock(pools_mutex_);

    uint64_t pool_id = key.id();

    // Check pool doesn't already exist
    if (pools_.find(pool_id) != pools_.end()) {
        return errors::POOL_ALREADY_INITIALIZED;
    }

    // Compute initial tick from sqrt price
    int32_t tick = get_tick_at_sqrt_ratio(sqrt_price_x96);

    // Initialize pool state
    PoolState state{};
    state.slot0.sqrt_price_x96 = sqrt_price_x96;
    state.slot0.tick = tick;
    state.slot0.protocol_fee = 0;
    state.slot0.lp_fee = key.fee;
    state.slot0.unlocked = true;
    state.fee_growth_global0_x128 = 0;
    state.fee_growth_global1_x128 = 0;
    state.protocol_fees0 = 0;
    state.protocol_fees1 = 0;
    state.liquidity = 0;

    pools_[pool_id] = std::move(state);

    lock.unlock();

    // Call after_initialize hook
    if (hooks) {
        hooks->after_initialize(key, sqrt_price_x96, tick);
    }

    return tick;
}

// =============================================================================
// Swap Step Computation (Uniswap v3 math)
// =============================================================================

LXPool::SwapState LXPool::compute_swap_step(SwapState state, I128 sqrt_price_target_x96,
                                             uint32_t fee_pips, bool zero_for_one) {
    // No liquidity: move price directly to target
    if (state.liquidity <= 0) {
        state.sqrt_price_x96 = sqrt_price_target_x96;
        state.tick = get_tick_at_sqrt_ratio(sqrt_price_target_x96);
        return state;
    }

    bool exact_in = state.amount_remaining > 0;
    I128 amount_remaining = abs128(state.amount_remaining);

    // Price delta for this step
    I128 sqrt_price_delta = zero_for_one
        ? state.sqrt_price_x96 - sqrt_price_target_x96
        : sqrt_price_target_x96 - state.sqrt_price_x96;

    if (sqrt_price_delta <= 0) {
        return state;
    }

    I128 amount_in = 0;
    I128 amount_out = 0;
    I128 fee_amount = 0;

    // Compute max amounts for full step
    // For zero_for_one: amount0_in = L * (1/sqrt_b - 1/sqrt_a)
    //                   amount1_out = L * (sqrt_a - sqrt_b)
    // For one_for_zero: amount1_in = L * (sqrt_b - sqrt_a)
    //                   amount0_out = L * (1/sqrt_a - 1/sqrt_b)

    if (zero_for_one) {
        // amount1 out = L * delta_sqrt_price / Q96
        I128 amount1_max = mul_div(state.liquidity, sqrt_price_delta, Q96);

        // amount0 in = L * delta_sqrt_price * Q96 / (sqrt_a * sqrt_b)
        I128 amount0_max = mul_div_up(
            mul_div(state.liquidity, sqrt_price_delta, sqrt_price_target_x96),
            Q96, state.sqrt_price_x96
        );

        if (exact_in) {
            // Deduct fee from input
            I128 amount_in_after_fee = mul_div(
                state.amount_remaining,
                FEE_DENOMINATOR - fee_pips,
                FEE_DENOMINATOR
            );

            if (amount_in_after_fee >= amount0_max) {
                // Full step: use all available in this range
                amount_in = amount0_max;
                amount_out = amount1_max;
                state.sqrt_price_x96 = sqrt_price_target_x96;
            } else {
                // Partial step: compute new price from amount
                amount_in = amount_in_after_fee;
                // Interpolate price based on proportion used
                I128 ratio = mul_div(amount_in, Q96, amount0_max);
                state.sqrt_price_x96 = state.sqrt_price_x96 - mul_div(sqrt_price_delta, ratio, Q96);
                // Compute output for actual step
                amount_out = mul_div(state.liquidity,
                                      state.sqrt_price_x96 - sqrt_price_target_x96 + sqrt_price_delta - mul_div(sqrt_price_delta, ratio, Q96),
                                      Q96);
                // Simplify: proportional output
                amount_out = mul_div(amount1_max, ratio, Q96);
            }
            fee_amount = mul_div_up(amount_in, fee_pips, FEE_DENOMINATOR - fee_pips);
            state.amount_remaining -= (amount_in + fee_amount);
            state.amount_calculated += amount_out;
        } else {
            // Exact output
            if (amount_remaining >= amount1_max) {
                amount_out = amount1_max;
                amount_in = amount0_max;
                state.sqrt_price_x96 = sqrt_price_target_x96;
            } else {
                amount_out = amount_remaining;
                I128 ratio = mul_div(amount_out, Q96, amount1_max);
                state.sqrt_price_x96 = state.sqrt_price_x96 - mul_div(sqrt_price_delta, ratio, Q96);
                amount_in = mul_div_up(amount0_max, ratio, Q96);
            }
            fee_amount = mul_div_up(amount_in, fee_pips, FEE_DENOMINATOR - fee_pips);
            state.amount_remaining += amount_out;
            state.amount_calculated += (amount_in + fee_amount);
        }
    } else {
        // one_for_zero direction
        // amount0 out = L * delta_sqrt_price * Q96 / (sqrt_a * sqrt_b)
        I128 amount0_max = mul_div(
            mul_div(state.liquidity, sqrt_price_delta, state.sqrt_price_x96),
            Q96, sqrt_price_target_x96
        );

        // amount1 in = L * delta_sqrt_price / Q96
        I128 amount1_max = mul_div_up(state.liquidity, sqrt_price_delta, Q96);

        if (exact_in) {
            I128 amount_in_after_fee = mul_div(
                state.amount_remaining,
                FEE_DENOMINATOR - fee_pips,
                FEE_DENOMINATOR
            );

            if (amount_in_after_fee >= amount1_max) {
                amount_in = amount1_max;
                amount_out = amount0_max;
                state.sqrt_price_x96 = sqrt_price_target_x96;
            } else {
                amount_in = amount_in_after_fee;
                I128 ratio = mul_div(amount_in, Q96, amount1_max);
                state.sqrt_price_x96 = state.sqrt_price_x96 + mul_div(sqrt_price_delta, ratio, Q96);
                amount_out = mul_div(amount0_max, ratio, Q96);
            }
            fee_amount = mul_div_up(amount_in, fee_pips, FEE_DENOMINATOR - fee_pips);
            state.amount_remaining -= (amount_in + fee_amount);
            state.amount_calculated += amount_out;
        } else {
            if (amount_remaining >= amount0_max) {
                amount_out = amount0_max;
                amount_in = amount1_max;
                state.sqrt_price_x96 = sqrt_price_target_x96;
            } else {
                amount_out = amount_remaining;
                I128 ratio = mul_div(amount_out, Q96, amount0_max);
                state.sqrt_price_x96 = state.sqrt_price_x96 + mul_div(sqrt_price_delta, ratio, Q96);
                amount_in = mul_div_up(amount1_max, ratio, Q96);
            }
            fee_amount = mul_div_up(amount_in, fee_pips, FEE_DENOMINATOR - fee_pips);
            state.amount_remaining += amount_out;
            state.amount_calculated += (amount_in + fee_amount);
        }
    }

    state.tick = get_tick_at_sqrt_ratio(state.sqrt_price_x96);
    return state;
}

// =============================================================================
// Swap
// =============================================================================

// Standalone swap (no flash context)
BalanceDelta LXPool::swap(const PoolKey& key, const SwapParams& params,
                          const std::vector<uint8_t>& hook_data) {
    FlashContext dummy_ctx;
    return swap(dummy_ctx, key, params, hook_data);
}

// Swap with explicit flash context
BalanceDelta LXPool::swap(FlashContext& ctx, const PoolKey& key, const SwapParams& params,
                          const std::vector<uint8_t>& hook_data) {
    // Call before_swap hook
    IHooks* hooks = get_hooks(key);
    if (hooks && !hooks->before_swap(key, params)) {
        return {0, 0};
    }

    std::unique_lock lock(pools_mutex_);

    PoolState* pool = get_pool(key);
    if (!pool) {
        return {0, 0};
    }

    // Reentrancy check
    if (!pool->slot0.unlocked) {
        return {0, 0};
    }
    pool->slot0.unlocked = false;

    // Determine price limit
    I128 sqrt_price_limit = params.sqrt_price_limit;
    if (sqrt_price_limit == 0) {
        sqrt_price_limit = params.zero_for_one
            ? tick_math::MIN_SQRT_RATIO + 1
            : tick_math::MAX_SQRT_RATIO - 1;
    }

    // Validate price limit
    if (params.zero_for_one) {
        if (sqrt_price_limit >= pool->slot0.sqrt_price_x96 ||
            sqrt_price_limit <= tick_math::MIN_SQRT_RATIO) {
            pool->slot0.unlocked = true;
            return {0, 0};
        }
    } else {
        if (sqrt_price_limit <= pool->slot0.sqrt_price_x96 ||
            sqrt_price_limit >= tick_math::MAX_SQRT_RATIO) {
            pool->slot0.unlocked = true;
            return {0, 0};
        }
    }

    // Initialize swap state
    SwapState state{};
    state.amount_remaining = params.amount_specified;
    state.amount_calculated = 0;
    state.sqrt_price_x96 = pool->slot0.sqrt_price_x96;
    state.tick = pool->slot0.tick;
    state.liquidity = pool->liquidity;

    // Fee for this swap
    uint32_t swap_fee = pool->slot0.lp_fee;

    // Track total fees for fee_growth_global accumulation
    I128 total_fee_amount = 0;

    // Main swap loop: iterate through tick ranges
    // Limit iterations to prevent infinite loops
    int max_iterations = 1000;
    while (state.amount_remaining != 0 && state.sqrt_price_x96 != sqrt_price_limit && max_iterations-- > 0) {

        // Find next initialized tick
        int32_t next_tick;
        bool found_tick = false;

        if (params.zero_for_one) {
            // Moving down: find highest initialized tick below current
            auto it = pool->ticks.lower_bound(state.tick);
            if (it != pool->ticks.begin()) {
                --it;
                // Find first initialized tick going backwards
                while (!it->second.initialized && it != pool->ticks.begin()) --it;
                if (it->second.initialized && it->first < state.tick) {
                    next_tick = it->first;
                    found_tick = true;
                }
            }
            if (!found_tick) {
                // No more initialized ticks below; use price limit tick
                next_tick = get_tick_at_sqrt_ratio(sqrt_price_limit);
                if (next_tick < tick_math::MIN_TICK) next_tick = tick_math::MIN_TICK;
            }
        } else {
            // Moving up: find lowest initialized tick above current
            auto it = pool->ticks.upper_bound(state.tick);
            while (it != pool->ticks.end() && !it->second.initialized) ++it;
            if (it != pool->ticks.end() && it->first > state.tick) {
                next_tick = it->first;
                found_tick = true;
            } else {
                next_tick = get_tick_at_sqrt_ratio(sqrt_price_limit);
                if (next_tick > tick_math::MAX_TICK) next_tick = tick_math::MAX_TICK;
            }
        }

        // Snap to tick spacing (rounding in swap direction)
        if (params.zero_for_one) {
            // Round down for zero_for_one
            next_tick = (next_tick / key.tick_spacing) * key.tick_spacing;
        } else {
            // Round up for one_for_zero
            next_tick = ((next_tick + key.tick_spacing - 1) / key.tick_spacing) * key.tick_spacing;
        }

        // Compute sqrt price at next tick
        I128 sqrt_price_next = get_sqrt_ratio_at_tick(next_tick);

        // Clamp to price limit
        I128 sqrt_price_target;
        if (params.zero_for_one) {
            sqrt_price_target = sqrt_price_next < sqrt_price_limit
                ? sqrt_price_limit : sqrt_price_next;
        } else {
            sqrt_price_target = sqrt_price_next > sqrt_price_limit
                ? sqrt_price_limit : sqrt_price_next;
        }

        // Ensure we're actually moving
        if ((params.zero_for_one && sqrt_price_target >= state.sqrt_price_x96) ||
            (!params.zero_for_one && sqrt_price_target <= state.sqrt_price_x96)) {
            // Price target is not in swap direction; use limit
            sqrt_price_target = sqrt_price_limit;
        }

        // Store old price for comparison
        I128 sqrt_price_before = state.sqrt_price_x96;

        // Compute swap within this step
        state = compute_swap_step(state, sqrt_price_target, swap_fee, params.zero_for_one);

        // If price didn't move and we still have amount, we're done
        if (state.sqrt_price_x96 == sqrt_price_before && state.amount_remaining != 0) {
            break;
        }

        // Cross tick if reached exactly and it was initialized
        if (state.sqrt_price_x96 == sqrt_price_next && found_tick) {
            auto tick_it = pool->ticks.find(next_tick);
            if (tick_it != pool->ticks.end() && tick_it->second.initialized) {
                // Flip fee growth outside when crossing
                tick_it->second.fee_growth_outside0_x128 =
                    pool->fee_growth_global0_x128 - tick_it->second.fee_growth_outside0_x128;
                tick_it->second.fee_growth_outside1_x128 =
                    pool->fee_growth_global1_x128 - tick_it->second.fee_growth_outside1_x128;

                // Update liquidity
                I128 liquidity_net = tick_it->second.liquidity_net;
                if (params.zero_for_one) {
                    state.liquidity -= liquidity_net;
                } else {
                    state.liquidity += liquidity_net;
                }
            }
            // Update tick (move past the crossed tick)
            state.tick = params.zero_for_one ? next_tick - 1 : next_tick;
        }
    }

    // Persist state changes
    pool->slot0.sqrt_price_x96 = state.sqrt_price_x96;
    pool->slot0.tick = state.tick;
    pool->liquidity = state.liquidity;

    // Calculate balance delta
    bool exact_in = params.amount_specified > 0;
    BalanceDelta delta{};

    if (params.zero_for_one) {
        // Sold token0, bought token1
        delta.amount0 = exact_in
            ? params.amount_specified - state.amount_remaining
            : state.amount_calculated;
        delta.amount1 = exact_in
            ? -state.amount_calculated
            : params.amount_specified - state.amount_remaining;
    } else {
        // Sold token1, bought token0
        delta.amount0 = exact_in
            ? -state.amount_calculated
            : params.amount_specified - state.amount_remaining;
        delta.amount1 = exact_in
            ? params.amount_specified - state.amount_remaining
            : state.amount_calculated;
    }

    // Unlock pool
    pool->slot0.unlocked = true;

    // Update flash accounting deltas
    if (locked_) {
        currency_deltas_[currency_hash(key.currency0)] += delta.amount0;
        currency_deltas_[currency_hash(key.currency1)] += delta.amount1;
    }

    // Update statistics
    total_swaps_.fetch_add(1, std::memory_order_relaxed);

    lock.unlock();

    // Call after_swap hook
    if (hooks) {
        hooks->after_swap(key, params, delta);
    }

    return delta;
}

// =============================================================================
// Modify Liquidity
// =============================================================================

BalanceDelta LXPool::modify_liquidity(const PoolKey& key, const ModifyLiquidityParams& params,
                                       const std::vector<uint8_t>& hook_data) {
    // Validate tick range
    if (params.tick_lower >= params.tick_upper) {
        return {0, 0};
    }
    if (params.tick_lower < tick_math::MIN_TICK ||
        params.tick_upper > tick_math::MAX_TICK) {
        return {0, 0};
    }
    if (params.tick_lower % key.tick_spacing != 0 ||
        params.tick_upper % key.tick_spacing != 0) {
        return {0, 0};
    }

    // Call before hook
    IHooks* hooks = get_hooks(key);
    if (hooks && !hooks->before_modify_liquidity(key, params)) {
        return {0, 0};
    }

    std::unique_lock lock(pools_mutex_);

    PoolState* pool = get_pool(key);
    if (!pool) {
        return {0, 0};
    }

    int32_t tick_current = pool->slot0.tick;
    I128 liquidity_delta = params.liquidity_delta;

    // Update lower tick
    TickInfo& lower = pool->ticks[params.tick_lower];
    I128 lower_gross_before = lower.liquidity_gross;
    lower.liquidity_gross += abs128(liquidity_delta);
    lower.liquidity_net += liquidity_delta;

    // Initialize tick if first liquidity
    if (lower_gross_before == 0 && lower.liquidity_gross > 0) {
        lower.initialized = true;
        if (tick_current >= params.tick_lower) {
            lower.fee_growth_outside0_x128 = pool->fee_growth_global0_x128;
            lower.fee_growth_outside1_x128 = pool->fee_growth_global1_x128;
        }
    } else if (lower.liquidity_gross == 0) {
        lower.initialized = false;
    }

    // Update upper tick
    TickInfo& upper = pool->ticks[params.tick_upper];
    I128 upper_gross_before = upper.liquidity_gross;
    upper.liquidity_gross += abs128(liquidity_delta);
    upper.liquidity_net -= liquidity_delta;  // Opposite sign for upper

    if (upper_gross_before == 0 && upper.liquidity_gross > 0) {
        upper.initialized = true;
        if (tick_current >= params.tick_upper) {
            upper.fee_growth_outside0_x128 = pool->fee_growth_global0_x128;
            upper.fee_growth_outside1_x128 = pool->fee_growth_global1_x128;
        }
    } else if (upper.liquidity_gross == 0) {
        upper.initialized = false;
    }

    // Update global liquidity if position is in range
    if (tick_current >= params.tick_lower && tick_current < params.tick_upper) {
        pool->liquidity += liquidity_delta;
    }

    // Compute fee growth inside range
    I128 fee_below0, fee_below1;
    if (tick_current >= params.tick_lower) {
        fee_below0 = lower.fee_growth_outside0_x128;
        fee_below1 = lower.fee_growth_outside1_x128;
    } else {
        fee_below0 = pool->fee_growth_global0_x128 - lower.fee_growth_outside0_x128;
        fee_below1 = pool->fee_growth_global1_x128 - lower.fee_growth_outside1_x128;
    }

    I128 fee_above0, fee_above1;
    if (tick_current < params.tick_upper) {
        fee_above0 = upper.fee_growth_outside0_x128;
        fee_above1 = upper.fee_growth_outside1_x128;
    } else {
        fee_above0 = pool->fee_growth_global0_x128 - upper.fee_growth_outside0_x128;
        fee_above1 = pool->fee_growth_global1_x128 - upper.fee_growth_outside1_x128;
    }

    I128 fee_inside0 = pool->fee_growth_global0_x128 - fee_below0 - fee_above0;
    I128 fee_inside1 = pool->fee_growth_global1_x128 - fee_below1 - fee_above1;

    // Update position
    Address owner{};  // Would be caller address in production
    uint64_t pos_key = position_key(owner, params.tick_lower, params.tick_upper, params.salt);
    PositionInfo& pos = pool->positions[pos_key];

    // Calculate fees owed
    I128 tokens_owed0 = 0;
    I128 tokens_owed1 = 0;
    if (pos.liquidity > 0) {
        tokens_owed0 = mul_div(
            fee_inside0 - pos.fee_growth_inside0_last_x128,
            pos.liquidity,
            Q128
        );
        tokens_owed1 = mul_div(
            fee_inside1 - pos.fee_growth_inside1_last_x128,
            pos.liquidity,
            Q128
        );
    }

    // Update position state
    pos.liquidity += liquidity_delta;
    pos.fee_growth_inside0_last_x128 = fee_inside0;
    pos.fee_growth_inside1_last_x128 = fee_inside1;
    pos.tokens_owed0 += tokens_owed0;
    pos.tokens_owed1 += tokens_owed1;

    // Calculate principal token amounts
    I128 sqrt_price_lower = get_sqrt_ratio_at_tick(params.tick_lower);
    I128 sqrt_price_upper = get_sqrt_ratio_at_tick(params.tick_upper);

    auto [amount0, amount1] = liquidity_math::get_amounts_for_liquidity(
        pool->slot0.sqrt_price_x96,
        sqrt_price_lower,
        sqrt_price_upper,
        abs128(liquidity_delta)
    );

    // Build delta: adding liquidity = pay in (positive), removing = receive (negative)
    BalanceDelta principal_delta{};
    if (liquidity_delta > 0) {
        principal_delta.amount0 = amount0;
        principal_delta.amount1 = amount1;
    } else {
        principal_delta.amount0 = -amount0;
        principal_delta.amount1 = -amount1;
    }

    // Add fees (always received = negative delta for pool)
    BalanceDelta fee_delta{-tokens_owed0, -tokens_owed1};
    BalanceDelta total_delta = principal_delta + fee_delta;

    // Update flash accounting
    if (locked_) {
        currency_deltas_[currency_hash(key.currency0)] += total_delta.amount0;
        currency_deltas_[currency_hash(key.currency1)] += total_delta.amount1;
    }

    // Update statistics
    total_liquidity_ops_.fetch_add(1, std::memory_order_relaxed);

    lock.unlock();

    // Call after hook
    if (hooks) {
        hooks->after_modify_liquidity(key, params, total_delta);
    }

    return total_delta;
}

// =============================================================================
// Donate
// =============================================================================

BalanceDelta LXPool::donate(const PoolKey& key, I128 amount0, I128 amount1,
                             const std::vector<uint8_t>& hook_data) {
    // Call before hook
    IHooks* hooks = get_hooks(key);
    if (hooks && !hooks->before_donate(key, amount0, amount1)) {
        return {0, 0};
    }

    std::unique_lock lock(pools_mutex_);

    PoolState* pool = get_pool(key);
    if (!pool) {
        return {0, 0};
    }

    // Cannot donate if no liquidity
    if (pool->liquidity <= 0) {
        return {0, 0};
    }

    // Distribute donation to fee growth (all in-range LPs benefit)
    if (amount0 > 0) {
        pool->fee_growth_global0_x128 += mul_div(amount0, Q128, pool->liquidity);
    }
    if (amount1 > 0) {
        pool->fee_growth_global1_x128 += mul_div(amount1, Q128, pool->liquidity);
    }

    BalanceDelta delta{amount0, amount1};

    // Update flash accounting
    if (locked_) {
        currency_deltas_[currency_hash(key.currency0)] += amount0;
        currency_deltas_[currency_hash(key.currency1)] += amount1;
    }

    lock.unlock();

    // Call after hook
    if (hooks) {
        hooks->after_donate(key, amount0, amount1);
    }

    return delta;
}

// =============================================================================
// Flash Accounting (Uniswap v4 pattern)
// =============================================================================

void LXPool::lock(LockCallback callback) {
    if (locked_) {
        throw std::runtime_error("LXPool: already locked (reentrancy)");
    }

    locked_ = true;
    currency_deltas_.clear();

    try {
        callback();
    } catch (...) {
        locked_ = false;
        currency_deltas_.clear();
        throw;
    }

    // Verify all deltas settled to zero
    for (const auto& [hash, delta] : currency_deltas_) {
        if (delta != 0) {
            locked_ = false;
            currency_deltas_.clear();
            throw std::runtime_error("LXPool: unsettled currency delta");
        }
    }

    locked_ = false;
    currency_deltas_.clear();
}

void LXPool::take(const Currency& currency, const Address& to, I128 amount) {
    if (!locked_) {
        throw std::runtime_error("LXPool: not locked");
    }
    // Taking creates debt (positive delta = pool is owed)
    currency_deltas_[currency_hash(currency)] += amount;
    // In production: call ERC20.transfer(to, amount)
    (void)to;
}

I128 LXPool::settle(const Currency& currency) {
    if (!locked_) {
        throw std::runtime_error("LXPool: not locked");
    }
    uint64_t h = currency_hash(currency);
    I128 delta = currency_deltas_[h];
    // In production: receive tokens and verify balance increased
    currency_deltas_[h] = 0;
    return delta;
}

void LXPool::sync(const Currency& currency) {
    if (!locked_) {
        throw std::runtime_error("LXPool: not locked");
    }
    // Sync: update internal balance tracking after external transfer
    // In production: read actual token balance and reconcile
    uint64_t h = currency_hash(currency);
    currency_deltas_[h] = 0;
}

// =============================================================================
// Query Operations
// =============================================================================

std::optional<Slot0> LXPool::get_slot0(const PoolKey& key) const {
    std::shared_lock lock(pools_mutex_);
    const PoolState* pool = get_pool(key);
    return pool ? std::optional{pool->slot0} : std::nullopt;
}

std::optional<I128> LXPool::get_liquidity(const PoolKey& key) const {
    std::shared_lock lock(pools_mutex_);
    const PoolState* pool = get_pool(key);
    return pool ? std::optional{pool->liquidity} : std::nullopt;
}

std::optional<PositionInfo> LXPool::get_position(const PoolKey& key,
                                                   const Address& owner,
                                                   int32_t tick_lower,
                                                   int32_t tick_upper,
                                                   uint64_t salt) const {
    std::shared_lock lock(pools_mutex_);
    const PoolState* pool = get_pool(key);
    if (!pool) return std::nullopt;

    uint64_t pos_key = position_key(owner, tick_lower, tick_upper, salt);
    auto it = pool->positions.find(pos_key);
    return it != pool->positions.end() ? std::optional{it->second} : std::nullopt;
}

bool LXPool::pool_exists(const PoolKey& key) const {
    std::shared_lock lock(pools_mutex_);
    return pools_.find(key.id()) != pools_.end();
}

// =============================================================================
// Protocol Fee Management
// =============================================================================

void LXPool::set_protocol_fee(const PoolKey& key, uint32_t new_fee) {
    std::unique_lock lock(pools_mutex_);
    PoolState* pool = get_pool(key);
    if (pool) {
        pool->slot0.protocol_fee = new_fee;
    }
}

BalanceDelta LXPool::collect_protocol(const PoolKey& key, const Address& recipient) {
    std::unique_lock lock(pools_mutex_);
    PoolState* pool = get_pool(key);
    if (!pool) {
        return {0, 0};
    }

    I128 amount0 = pool->protocol_fees0;
    I128 amount1 = pool->protocol_fees1;

    pool->protocol_fees0 = 0;
    pool->protocol_fees1 = 0;

    // In production: transfer tokens to recipient
    (void)recipient;

    // Negative delta: pool pays out
    return {-amount0, -amount1};
}

// =============================================================================
// Hook Registration
// =============================================================================

void LXPool::register_hooks(const Address& hook_addr, IHooks* hooks) {
    if (!hooks || is_zero_address(hook_addr)) return;
    std::unique_lock lock(hooks_mutex_);
    hooks_[address_hash(hook_addr)] = hooks;
}

void LXPool::unregister_hooks(const Address& hook_addr) {
    std::unique_lock lock(hooks_mutex_);
    hooks_.erase(address_hash(hook_addr));
}

// =============================================================================
// Statistics
// =============================================================================

LXPool::Stats LXPool::get_stats() const {
    std::shared_lock lock(pools_mutex_);
    return Stats{
        static_cast<uint64_t>(pools_.size()),
        total_swaps_.load(std::memory_order_relaxed),
        total_liquidity_ops_.load(std::memory_order_relaxed),
        0,  // total_volume0_x18 - would track per-swap in production
        0   // total_volume1_x18
    };
}

} // namespace lux
