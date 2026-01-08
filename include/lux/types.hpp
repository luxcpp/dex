#ifndef LUX_TYPES_HPP
#define LUX_TYPES_HPP

#include <cstdint>
#include <array>
#include <string>
#include <cstring>
#include <vector>

namespace lux {

// =============================================================================
// LP-Aligned Precompile Addresses (EVM 20-byte addresses)
// Format: 0x0000000000000000000000000000000000LPNUM
// =============================================================================

using Address = std::array<uint8_t, 20>;

namespace addresses {

// AMM Core (LP-9010 series)
constexpr Address LX_POOL   = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x10};  // LP-9010
constexpr Address LX_ORACLE = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x11};  // LP-9011
constexpr Address LX_ROUTER = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x12};  // LP-9012
constexpr Address LX_HOOKS  = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x13};  // LP-9013
constexpr Address LX_FLASH  = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x14};  // LP-9014

// CLOB (LP-9020 series)
constexpr Address LX_BOOK   = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x20};  // LP-9020

// Custody/Margin (LP-9030 series)
constexpr Address LX_VAULT  = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x30};  // LP-9030

// Price Feeds (LP-9040 series)
constexpr Address LX_FEED   = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x40};  // LP-9040

// Lending (LP-9050 series)
constexpr Address LX_LEND   = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x50};  // LP-9050

// Self-Repaying Loans (LP-9060 series)
constexpr Address LX_LIQUID = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x60};  // LP-9060

// Cross-chain (LP-6010 series)
constexpr Address TELEPORT  = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x60,0x10};  // LP-6010

// Helper to create address from LP number
constexpr Address from_lp(uint16_t lp_num) {
    Address addr = {};
    addr[18] = static_cast<uint8_t>((lp_num >> 8) & 0xFF);
    addr[19] = static_cast<uint8_t>(lp_num & 0xFF);
    return addr;
}

// Extract LP number from address
constexpr uint16_t to_lp(const Address& addr) {
    return (static_cast<uint16_t>(addr[18]) << 8) | static_cast<uint16_t>(addr[19]);
}

// Check if address is a DEX precompile (LP-9xxx)
constexpr bool is_dex_precompile(const Address& addr) {
    // Check first 17 bytes are zero
    for (size_t i = 0; i < 17; ++i) {
        if (addr[i] != 0) return false;
    }
    // Check LP-9xxx range (0x9010-0x90FF)
    return addr[17] == 0 && (addr[18] & 0xF0) == 0x90;
}

} // namespace addresses

// =============================================================================
// Fixed-Point Arithmetic (X18 = 18 decimal places)
// =============================================================================

using X18 = __int128;  // 128-bit for X18 arithmetic
using I128 = __int128;
using U128 = unsigned __int128;

constexpr I128 X18_ONE = 1000000000000000000LL;  // 1e18
constexpr I128 X18_HALF = 500000000000000000LL;  // 0.5e18

// Safe X18 operations
namespace x18 {

inline I128 mul(I128 a, I128 b) {
    return (a * b) / X18_ONE;
}

inline I128 div(I128 a, I128 b) {
    return (a * X18_ONE) / b;
}

inline I128 from_double(double v) {
    return static_cast<I128>(v * static_cast<double>(X18_ONE));
}

inline double to_double(I128 v) {
    return static_cast<double>(v) / static_cast<double>(X18_ONE);
}

inline I128 from_int(int64_t v) {
    return static_cast<I128>(v) * X18_ONE;
}

inline int64_t to_int(I128 v) {
    return static_cast<int64_t>(v / X18_ONE);
}

// Square root via Newton-Raphson
inline I128 sqrt(I128 x) {
    if (x == 0) return 0;
    I128 z = (x + X18_ONE) / 2;
    I128 y = x;
    while (z < y) {
        y = z;
        z = (x18::div(x, z) + z) / 2;
    }
    return y;
}

} // namespace x18

// =============================================================================
// Currency Type (Token Address)
// =============================================================================

struct Currency {
    Address addr;

    Currency() : addr{} {}
    explicit Currency(const Address& a) : addr(a) {}

    bool is_native() const {
        for (auto b : addr) if (b != 0) return false;
        return true;
    }

    bool operator==(const Currency& other) const { return addr == other.addr; }
    bool operator!=(const Currency& other) const { return addr != other.addr; }
    bool operator<(const Currency& other) const { return addr < other.addr; }
};

// Native LUX token (address(0))
inline const Currency NATIVE_LUX{};

// =============================================================================
// Pool Key (Unique Pool Identifier)
// =============================================================================

struct PoolKey {
    Currency currency0;      // Sorted: currency0 < currency1
    Currency currency1;
    uint32_t fee;            // Fee in hundredths of a bip (100 = 0.01%)
    int32_t tick_spacing;    // Tick spacing for concentrated liquidity
    Address hooks;           // Hook contract address (0 = no hooks)

    // Compute pool ID hash
    uint64_t id() const {
        // Simple hash combining all fields
        uint64_t h = 0;
        for (auto b : currency0.addr) h = h * 31 + b;
        for (auto b : currency1.addr) h = h * 31 + b;
        h = h * 31 + fee;
        h = h * 31 + static_cast<uint64_t>(tick_spacing);
        for (auto b : hooks) h = h * 31 + b;
        return h;
    }

    bool operator==(const PoolKey& other) const {
        return currency0 == other.currency0 &&
               currency1 == other.currency1 &&
               fee == other.fee &&
               tick_spacing == other.tick_spacing &&
               hooks == other.hooks;
    }
};

// Standard fee tiers (in hundredths of a bip)
namespace fees {
constexpr uint32_t FEE_001 = 100;     // 0.01%
constexpr uint32_t FEE_005 = 500;     // 0.05%
constexpr uint32_t FEE_030 = 3000;    // 0.30%
constexpr uint32_t FEE_100 = 10000;   // 1.00%
constexpr uint32_t FEE_MAX = 100000;  // 10.00%
}

// Standard tick spacings
namespace tick_spacings {
constexpr int32_t TICK_SPACING_001 = 1;
constexpr int32_t TICK_SPACING_005 = 10;
constexpr int32_t TICK_SPACING_030 = 60;
constexpr int32_t TICK_SPACING_100 = 200;
}

// =============================================================================
// Balance Delta (Signed Token Amounts)
// =============================================================================

struct BalanceDelta {
    I128 amount0;  // X18
    I128 amount1;  // X18

    BalanceDelta operator+(const BalanceDelta& other) const {
        return {amount0 + other.amount0, amount1 + other.amount1};
    }

    BalanceDelta operator-(const BalanceDelta& other) const {
        return {amount0 - other.amount0, amount1 - other.amount1};
    }

    BalanceDelta operator-() const {
        return {-amount0, -amount1};
    }
};

// =============================================================================
// Swap Parameters
// =============================================================================

struct SwapParams {
    bool zero_for_one;       // true = sell token0 for token1
    I128 amount_specified;   // X18: positive = exact input, negative = exact output
    I128 sqrt_price_limit;   // X96: price limit (0 = no limit)
};

// =============================================================================
// Modify Liquidity Parameters
// =============================================================================

struct ModifyLiquidityParams {
    int32_t tick_lower;
    int32_t tick_upper;
    I128 liquidity_delta;    // X18: positive = add, negative = remove
    uint64_t salt;           // For multiple positions at same range
};

// =============================================================================
// Account Identifier (for Vault)
// =============================================================================

struct LXAccount {
    Address main;           // Main wallet address
    uint16_t subaccount_id; // Subaccount number (0 = default)

    bool operator==(const LXAccount& other) const {
        return main == other.main && subaccount_id == other.subaccount_id;
    }

    uint64_t hash() const {
        uint64_t h = subaccount_id;
        for (auto b : main) h = h * 31 + b;
        return h;
    }
};

// =============================================================================
// Position & Margin Types
// =============================================================================

enum class MarginMode : uint8_t {
    CROSS = 0,
    ISOLATED = 1
};

enum class PositionSide : uint8_t {
    LONG = 0,
    SHORT = 1
};

struct LXPosition {
    uint32_t market_id;
    PositionSide side;
    I128 size_x18;
    I128 entry_px_x18;
    I128 unrealized_pnl_x18;
    I128 accumulated_funding_x18;
    uint64_t last_funding_time;
};

struct LXMarginInfo {
    I128 total_collateral_x18;
    I128 used_margin_x18;
    I128 free_margin_x18;
    I128 margin_ratio_x18;
    I128 maintenance_margin_x18;
    bool liquidatable;
};

// =============================================================================
// Order Types (CLOB)
// =============================================================================

enum class TIF : uint8_t {
    GTC = 0,   // Good Till Cancel
    IOC = 1,   // Immediate Or Cancel
    ALO = 2    // Add Liquidity Only (post-only)
};

enum class OrderKind : uint8_t {
    LIMIT = 0,
    MARKET = 1,
    STOP_MARKET = 2,
    STOP_LIMIT = 3,
    TAKE_MARKET = 4,
    TAKE_LIMIT = 5
};

enum class GroupType : uint8_t {
    NONE = 0,
    OCO = 1,       // One Cancels Other
    BRACKET = 2    // Bracket order (TP + SL)
};

enum class ActionType : uint8_t {
    PLACE = 0,
    CANCEL = 1,
    CANCEL_BY_CLOID = 2,
    MODIFY = 3,
    TWAP_CREATE = 4,
    TWAP_CANCEL = 5,
    SCHEDULE_CANCEL = 6,
    NOOP = 7,
    RESERVE_WEIGHT = 8
};

struct LXOrder {
    uint32_t market_id;
    bool is_buy;
    OrderKind kind;
    I128 size_x18;
    I128 limit_px_x18;
    I128 trigger_px_x18;
    bool reduce_only;
    TIF tif;
    std::array<uint8_t, 16> cloid;  // Client order ID (UUID)
    std::array<uint8_t, 16> group_id;
    GroupType group_type;
};

struct LXAction {
    ActionType action_type;
    uint64_t nonce;
    uint64_t expires_after;
    std::vector<uint8_t> data;
};

struct LXPlaceResult {
    uint64_t oid;           // Order ID
    uint8_t status;         // Order status
    I128 filled_size_x18;
    I128 avg_px_x18;
};

struct LXL1 {
    I128 best_bid_px_x18;
    I128 best_bid_sz_x18;
    I128 best_ask_px_x18;
    I128 best_ask_sz_x18;
    I128 last_trade_px_x18;
};

// =============================================================================
// Price Feed Types
// =============================================================================

struct LXMarkPrice {
    I128 index_px_x18;
    I128 mark_px_x18;
    I128 premium_x18;
    uint64_t timestamp;
};

struct LXFundingRate {
    I128 rate_x18;
    uint64_t next_funding_time;
};

// Oracle source types
enum class PriceSource : uint8_t {
    BINANCE = 0,
    COINBASE = 1,
    OKX = 2,
    BYBIT = 3,
    UNISWAP = 4,
    LXPOOL = 5,
    CHAINLINK = 6,
    PYTH = 7,
    CUSTOM = 8
};

enum class AggregationMethod : uint8_t {
    MEDIAN = 0,
    TWAP = 1,
    VWAP = 2,
    TRIMMED_MEAN = 3,
    WEIGHTED_MEDIAN = 4
};

// =============================================================================
// Fill Flags (for settlement)
// =============================================================================

namespace fill_flags {
constexpr uint8_t NONE = 0;
constexpr uint8_t LIQUIDATION = 1 << 0;
constexpr uint8_t ADL = 1 << 1;
constexpr uint8_t REDUCE_ONLY = 1 << 2;
constexpr uint8_t POST_ONLY = 1 << 3;
constexpr uint8_t MAKER = 1 << 4;
constexpr uint8_t TAKER = 1 << 5;
}

// =============================================================================
// Error Codes
// =============================================================================

namespace errors {
constexpr int32_t OK = 0;
constexpr int32_t POOL_NOT_INITIALIZED = -1;
constexpr int32_t POOL_ALREADY_INITIALIZED = -2;
constexpr int32_t INVALID_TICK_RANGE = -3;
constexpr int32_t INSUFFICIENT_LIQUIDITY = -4;
constexpr int32_t PRICE_LIMIT_EXCEEDED = -5;
constexpr int32_t INVALID_CURRENCY = -6;
constexpr int32_t CURRENCIES_NOT_SORTED = -7;
constexpr int32_t INVALID_FEE = -8;
constexpr int32_t INSUFFICIENT_BALANCE = -10;
constexpr int32_t INSUFFICIENT_MARGIN = -11;
constexpr int32_t POSITION_NOT_FOUND = -12;
constexpr int32_t ORDER_NOT_FOUND = -13;
constexpr int32_t MARKET_NOT_FOUND = -14;
constexpr int32_t NOT_LIQUIDATABLE = -15;
constexpr int32_t PRICE_STALE = -20;
constexpr int32_t ORACLE_SOURCE_UNAVAILABLE = -21;
constexpr int32_t INVALID_PRICE = -22;
constexpr int32_t REENTRANCY = -30;
constexpr int32_t HOOK_FAILED = -31;
constexpr int32_t UNAUTHORIZED = -40;
}

} // namespace lux

#endif // LUX_TYPES_HPP
