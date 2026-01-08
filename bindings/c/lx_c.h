/**
 * lx_c.h - C Bindings for Full LX Stack
 *
 * LP-Aligned Precompile Addresses:
 *   LP-9010: LXPool   (AMM Pool Manager)
 *   LP-9011: LXOracle (Price Aggregation)
 *   LP-9012: LXRouter (Swap Routing)
 *   LP-9013: LXHooks  (Hook Registry)
 *   LP-9014: LXFlash  (Flash Loans)
 *   LP-9020: LXBook   (CLOB Matching)
 *   LP-9030: LXVault  (Custody & Margin)
 *   LP-9040: LXFeed   (Mark/Funding Prices)
 *   LP-9050: LXLend   (Lending Pool)
 *   LP-9060: LXLiquid (Self-Repaying Loans)
 */

#ifndef LX_C_H
#define LX_C_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 128-bit Integer Representation (hi/lo pairs for C compatibility)
 * ============================================================================= */

typedef struct {
    int64_t hi;    /* High 64 bits (signed for X18 arithmetic) */
    uint64_t lo;   /* Low 64 bits */
} lx_i128_t;

typedef struct {
    uint64_t hi;
    uint64_t lo;
} lx_u128_t;

/* X18 fixed-point constant: 1e18 = 1.0 */
#define LX_X18_ONE_LO 1000000000000000000ULL
#define LX_X18_ONE_HI 0

/* Helper to create i128 from int64 */
static inline lx_i128_t lx_i128_from_i64(int64_t v) {
    lx_i128_t r;
    r.hi = (v < 0) ? -1 : 0;
    r.lo = (uint64_t)v;
    return r;
}

/* =============================================================================
 * Address Type (20 bytes, EVM-compatible)
 * ============================================================================= */

typedef struct {
    uint8_t bytes[20];
} lx_address_t;

/* LP-Aligned precompile addresses */
static const lx_address_t LX_POOL_ADDRESS   = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x10}};
static const lx_address_t LX_ORACLE_ADDRESS = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x11}};
static const lx_address_t LX_ROUTER_ADDRESS = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x12}};
static const lx_address_t LX_HOOKS_ADDRESS  = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x13}};
static const lx_address_t LX_FLASH_ADDRESS  = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x14}};
static const lx_address_t LX_BOOK_ADDRESS   = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x20}};
static const lx_address_t LX_VAULT_ADDRESS  = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x30}};
static const lx_address_t LX_FEED_ADDRESS   = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x40}};
static const lx_address_t LX_LEND_ADDRESS   = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x50}};
static const lx_address_t LX_LIQUID_ADDRESS = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x90,0x60}};

/* =============================================================================
 * Opaque Handle Types
 * ============================================================================= */

typedef struct lx_s lx_t;

/* =============================================================================
 * Currency Type (Token Address)
 * ============================================================================= */

typedef struct {
    lx_address_t addr;
} lx_currency_t;

/* =============================================================================
 * Account Identifier
 * ============================================================================= */

typedef struct {
    lx_address_t main;       /* Main wallet address */
    uint16_t subaccount_id;  /* Subaccount number (0 = default) */
} lx_account_t;

/* =============================================================================
 * Pool Key (Unique Pool Identifier)
 * ============================================================================= */

typedef struct {
    lx_currency_t currency0;      /* Sorted: currency0 < currency1 */
    lx_currency_t currency1;
    uint32_t fee;                 /* Fee in hundredths of bip (100 = 0.01%) */
    int32_t tick_spacing;
    lx_address_t hooks;           /* Hook contract address (zero = no hooks) */
} lx_pool_key_t;

/* Standard fee tiers */
#define LX_FEE_001   100      /* 0.01% */
#define LX_FEE_005   500      /* 0.05% */
#define LX_FEE_030   3000     /* 0.30% */
#define LX_FEE_100   10000    /* 1.00% */

/* Standard tick spacings */
#define LX_TICK_SPACING_001  1
#define LX_TICK_SPACING_005  10
#define LX_TICK_SPACING_030  60
#define LX_TICK_SPACING_100  200

/* =============================================================================
 * Balance Delta (Signed Token Amounts)
 * ============================================================================= */

typedef struct {
    lx_i128_t amount0;  /* X18 */
    lx_i128_t amount1;  /* X18 */
} lx_balance_delta_t;

/* =============================================================================
 * Swap Parameters
 * ============================================================================= */

typedef struct {
    bool zero_for_one;         /* true = sell token0 for token1 */
    lx_i128_t amount_specified; /* X18: positive = exact input, negative = exact output */
    lx_i128_t sqrt_price_limit; /* X96: price limit (0 = no limit) */
} lx_swap_params_t;

/* =============================================================================
 * Modify Liquidity Parameters
 * ============================================================================= */

typedef struct {
    int32_t tick_lower;
    int32_t tick_upper;
    lx_i128_t liquidity_delta;  /* X18: positive = add, negative = remove */
    uint64_t salt;              /* For multiple positions at same range */
} lx_modify_params_t;

/* =============================================================================
 * Pool Slot0 State
 * ============================================================================= */

typedef struct {
    lx_i128_t sqrt_price_x96;   /* Current sqrt(price) as Q64.96 */
    int32_t tick;               /* Current tick */
    uint32_t protocol_fee;
    uint32_t lp_fee;
    bool unlocked;
} lx_slot0_t;

/* =============================================================================
 * Enums (Order/Trade Types)
 * ============================================================================= */

typedef enum {
    LX_TIF_GTC = 0,   /* Good Till Cancel */
    LX_TIF_IOC = 1,   /* Immediate Or Cancel */
    LX_TIF_ALO = 2    /* Add Liquidity Only (post-only) */
} lx_tif_t;

typedef enum {
    LX_ORDER_LIMIT = 0,
    LX_ORDER_MARKET = 1,
    LX_ORDER_STOP_MARKET = 2,
    LX_ORDER_STOP_LIMIT = 3,
    LX_ORDER_TAKE_MARKET = 4,
    LX_ORDER_TAKE_LIMIT = 5
} lx_order_kind_t;

typedef enum {
    LX_MARGIN_CROSS = 0,
    LX_MARGIN_ISOLATED = 1
} lx_margin_mode_t;

typedef enum {
    LX_SIDE_LONG = 0,
    LX_SIDE_SHORT = 1
} lx_position_side_t;

typedef enum {
    LX_STATUS_NEW = 0,
    LX_STATUS_OPEN = 1,
    LX_STATUS_FILLED = 2,
    LX_STATUS_CANCELLED = 3,
    LX_STATUS_REJECTED = 4,
    LX_STATUS_EXPIRED = 5,
    LX_STATUS_TRIGGERED = 6
} lx_order_status_t;

typedef enum {
    LX_PRICE_BINANCE = 0,
    LX_PRICE_COINBASE = 1,
    LX_PRICE_OKX = 2,
    LX_PRICE_BYBIT = 3,
    LX_PRICE_UNISWAP = 4,
    LX_PRICE_LXPOOL = 5,
    LX_PRICE_CHAINLINK = 6,
    LX_PRICE_PYTH = 7,
    LX_PRICE_CUSTOM = 8
} lx_price_source_t;

/* =============================================================================
 * LXBook Market Configuration (LP-9020)
 * ============================================================================= */

typedef struct {
    uint32_t market_id;
    uint64_t symbol_id;
    lx_currency_t base_currency;
    lx_currency_t quote_currency;
    lx_i128_t tick_size_x18;
    lx_i128_t lot_size_x18;
    lx_i128_t min_notional_x18;
    lx_i128_t max_order_size_x18;
    bool post_only_mode;
    bool reduce_only_mode;
    uint8_t status;            /* 0=inactive, 1=active, 2=cancel-only */
} lx_book_market_config_t;

/* =============================================================================
 * LXBook Order
 * ============================================================================= */

typedef struct {
    uint32_t market_id;
    bool is_buy;
    lx_order_kind_t kind;
    lx_i128_t size_x18;
    lx_i128_t limit_px_x18;
    lx_i128_t trigger_px_x18;
    bool reduce_only;
    lx_tif_t tif;
    uint8_t cloid[16];         /* Client order ID (UUID) */
    uint8_t group_id[16];
    uint8_t group_type;        /* 0=NONE, 1=OCO, 2=BRACKET */
} lx_order_t;

/* =============================================================================
 * LXBook Place Result
 * ============================================================================= */

typedef struct {
    uint64_t oid;              /* Order ID */
    uint8_t status;            /* lx_order_status_t */
    lx_i128_t filled_size_x18;
    lx_i128_t avg_px_x18;
} lx_place_result_t;

/* =============================================================================
 * LXBook L1 Data
 * ============================================================================= */

typedef struct {
    lx_i128_t best_bid_px_x18;
    lx_i128_t best_bid_sz_x18;
    lx_i128_t best_ask_px_x18;
    lx_i128_t best_ask_sz_x18;
    lx_i128_t last_trade_px_x18;
} lx_l1_t;

/* =============================================================================
 * LXVault Market Configuration (LP-9030)
 * ============================================================================= */

typedef struct {
    uint32_t market_id;
    lx_currency_t base_currency;
    lx_currency_t quote_currency;
    lx_i128_t initial_margin_x18;      /* e.g., 0.1 = 10% */
    lx_i128_t maintenance_margin_x18;  /* e.g., 0.05 = 5% */
    lx_i128_t max_leverage_x18;        /* e.g., 20 = 20x */
    lx_i128_t taker_fee_x18;
    lx_i128_t maker_fee_x18;
    lx_i128_t min_order_size_x18;
    lx_i128_t max_position_size_x18;
    bool reduce_only_mode;
    bool active;
} lx_vault_market_config_t;

/* =============================================================================
 * LXVault Position
 * ============================================================================= */

typedef struct {
    uint32_t market_id;
    lx_position_side_t side;
    lx_i128_t size_x18;
    lx_i128_t entry_px_x18;
    lx_i128_t unrealized_pnl_x18;
    lx_i128_t accumulated_funding_x18;
    uint64_t last_funding_time;
} lx_position_t;

/* =============================================================================
 * LXVault Margin Info
 * ============================================================================= */

typedef struct {
    lx_i128_t total_collateral_x18;
    lx_i128_t used_margin_x18;
    lx_i128_t free_margin_x18;
    lx_i128_t margin_ratio_x18;
    lx_i128_t maintenance_margin_x18;
    bool liquidatable;
} lx_margin_info_t;

/* =============================================================================
 * LXVault Liquidation Result
 * ============================================================================= */

typedef struct {
    lx_account_t liquidated;
    lx_account_t liquidator;
    uint32_t market_id;
    lx_i128_t size_x18;
    lx_i128_t price_x18;
    lx_i128_t penalty_x18;
    bool adl_triggered;
} lx_liquidation_result_t;

/* =============================================================================
 * LXFeed Mark Price (LP-9040)
 * ============================================================================= */

typedef struct {
    lx_i128_t index_px_x18;
    lx_i128_t mark_px_x18;
    lx_i128_t premium_x18;
    uint64_t timestamp;
} lx_mark_price_t;

/* =============================================================================
 * LXFeed Funding Rate
 * ============================================================================= */

typedef struct {
    lx_i128_t rate_x18;
    uint64_t next_funding_time;
} lx_funding_rate_t;

/* =============================================================================
 * LX Configuration
 * ============================================================================= */

typedef struct {
    size_t worker_threads;
    size_t max_batch_size;
    bool enable_hooks;
    bool enable_flash_loans;
    uint64_t funding_interval;
    lx_i128_t default_maker_fee_x18;
    lx_i128_t default_taker_fee_x18;
} lx_dex_config_t;

/* =============================================================================
 * Error Codes
 * ============================================================================= */

#define LX_OK                        0
#define LX_ERR_POOL_NOT_INITIALIZED  -1
#define LX_ERR_POOL_ALREADY_INIT     -2
#define LX_ERR_INVALID_TICK_RANGE    -3
#define LX_ERR_INSUFFICIENT_LIQUIDITY -4
#define LX_ERR_PRICE_LIMIT_EXCEEDED  -5
#define LX_ERR_INVALID_CURRENCY      -6
#define LX_ERR_CURRENCIES_NOT_SORTED -7
#define LX_ERR_INVALID_FEE           -8
#define LX_ERR_INSUFFICIENT_BALANCE  -10
#define LX_ERR_INSUFFICIENT_MARGIN   -11
#define LX_ERR_POSITION_NOT_FOUND    -12
#define LX_ERR_ORDER_NOT_FOUND       -13
#define LX_ERR_MARKET_NOT_FOUND      -14
#define LX_ERR_NOT_LIQUIDATABLE      -15
#define LX_ERR_PRICE_STALE           -20
#define LX_ERR_ORACLE_UNAVAILABLE    -21
#define LX_ERR_INVALID_PRICE         -22
#define LX_ERR_REENTRANCY            -30
#define LX_ERR_HOOK_FAILED           -31
#define LX_ERR_UNAUTHORIZED          -40
#define LX_ERR_NULL_POINTER          -100
#define LX_ERR_INTERNAL              -101

/* =============================================================================
 * LX Controller API
 * ============================================================================= */

/**
 * Create a new LX instance.
 * Returns NULL on failure.
 */
lx_t* lx_create(void);

/**
 * Create LX with configuration.
 */
lx_t* lx_create_with_config(const lx_dex_config_t* config);

/**
 * Destroy LX instance and free all resources.
 */
void lx_destroy(lx_t* dex);

/**
 * Initialize all components with default configuration.
 * Returns LX_OK on success.
 */
int32_t lx_initialize(lx_t* dex);

/**
 * Start the DEX (enables trading).
 * Returns LX_OK on success.
 */
int32_t lx_start(lx_t* dex);

/**
 * Stop the DEX (disables trading).
 * Returns LX_OK on success.
 */
int32_t lx_stop(lx_t* dex);

/**
 * Check if DEX is running.
 */
bool lx_is_running(const lx_t* dex);

/**
 * Get DEX version string.
 */
const char* lx_version(void);

/* =============================================================================
 * LXPool API (LP-9010) - AMM Pool Manager
 * ============================================================================= */

/**
 * Initialize a new pool.
 * @param sqrt_price_x96_hi High 64 bits of initial sqrt price (Q64.96)
 * @param sqrt_price_x96_lo Low 64 bits of initial sqrt price
 * @return Initial tick on success, or negative error code
 */
int32_t lxpool_initialize(lx_t* dex, const lx_pool_key_t* key,
                          int64_t sqrt_price_x96_hi, uint64_t sqrt_price_x96_lo);

/**
 * Execute a swap.
 * @return Balance delta (amounts moved)
 */
lx_balance_delta_t lxpool_swap(lx_t* dex, const lx_pool_key_t* key,
                               const lx_swap_params_t* params);

/**
 * Add or remove liquidity.
 * @return Balance delta for principal + fees
 */
lx_balance_delta_t lxpool_modify_liquidity(lx_t* dex, const lx_pool_key_t* key,
                                           const lx_modify_params_t* params);

/**
 * Donate tokens to in-range LPs.
 * @return Balance delta
 */
lx_balance_delta_t lxpool_donate(lx_t* dex, const lx_pool_key_t* key,
                                  lx_i128_t amount0, lx_i128_t amount1);

/**
 * Get pool slot0 state.
 * @param out Output slot0 structure
 * @return true if pool exists
 */
bool lxpool_get_slot0(const lx_t* dex, const lx_pool_key_t* key, lx_slot0_t* out);

/**
 * Get pool liquidity.
 * @param out Output liquidity value
 * @return true if pool exists
 */
bool lxpool_get_liquidity(const lx_t* dex, const lx_pool_key_t* key, lx_i128_t* out);

/**
 * Check if pool exists.
 */
bool lxpool_exists(const lx_t* dex, const lx_pool_key_t* key);

/**
 * Set protocol fee for a pool.
 */
int32_t lxpool_set_protocol_fee(lx_t* dex, const lx_pool_key_t* key, uint32_t new_fee);

/**
 * Collect protocol fees.
 */
lx_balance_delta_t lxpool_collect_protocol(lx_t* dex, const lx_pool_key_t* key,
                                            const lx_address_t* recipient);

/* =============================================================================
 * LXBook API (LP-9020) - CLOB Matching Engine
 * ============================================================================= */

/**
 * Create a new CLOB market.
 * @return LX_OK on success
 */
int32_t lxbook_create_market(lx_t* dex, const lx_book_market_config_t* config);

/**
 * Update market configuration.
 */
int32_t lxbook_update_market(lx_t* dex, const lx_book_market_config_t* config);

/**
 * Check if market exists.
 */
bool lxbook_market_exists(const lx_t* dex, uint32_t market_id);

/**
 * Get market status.
 * @return 0=inactive, 1=active, 2=cancel-only
 */
uint8_t lxbook_get_market_status(const lx_t* dex, uint32_t market_id);

/**
 * Place a new order.
 * @return Place result with order ID and fill info
 */
lx_place_result_t lxbook_place_order(lx_t* dex, const lx_account_t* sender,
                                      const lx_order_t* order);

/**
 * Cancel order by order ID.
 * @return LX_OK on success
 */
int32_t lxbook_cancel_order(lx_t* dex, const lx_account_t* sender,
                            uint32_t market_id, uint64_t oid);

/**
 * Cancel order by client order ID.
 * @param cloid 16-byte client order ID
 * @return LX_OK on success
 */
int32_t lxbook_cancel_by_cloid(lx_t* dex, const lx_account_t* sender,
                               uint32_t market_id, const uint8_t* cloid);

/**
 * Cancel all orders for account in market.
 * @return Number of orders cancelled, or negative error
 */
int32_t lxbook_cancel_all(lx_t* dex, const lx_account_t* sender, uint32_t market_id);

/**
 * Amend order price/size.
 */
lx_place_result_t lxbook_amend_order(lx_t* dex, const lx_account_t* sender,
                                      uint32_t market_id, uint64_t oid,
                                      lx_i128_t new_size_x18, lx_i128_t new_price_x18);

/**
 * Get L1 (best bid/ask) for market.
 */
lx_l1_t lxbook_get_l1(const lx_t* dex, uint32_t market_id);

/**
 * Get order count for account in market.
 */
size_t lxbook_order_count(const lx_t* dex, const lx_account_t* account,
                          uint32_t market_id);

/* =============================================================================
 * LXVault API (LP-9030) - Clearinghouse
 * ============================================================================= */

/**
 * Create a new perpetual market.
 */
int32_t lxvault_create_market(lx_t* dex, const lx_vault_market_config_t* config);

/**
 * Update market configuration.
 */
int32_t lxvault_update_market(lx_t* dex, const lx_vault_market_config_t* config);

/**
 * Deposit collateral.
 * @param amount_hi High 64 bits of amount (X18)
 * @param amount_lo Low 64 bits of amount
 * @return LX_OK on success
 */
int32_t lxvault_deposit(lx_t* dex, const lx_account_t* account,
                        const lx_currency_t* token,
                        int64_t amount_hi, uint64_t amount_lo);

/**
 * Withdraw collateral (checks margin requirements).
 */
int32_t lxvault_withdraw(lx_t* dex, const lx_account_t* account,
                         const lx_currency_t* token,
                         int64_t amount_hi, uint64_t amount_lo);

/**
 * Internal transfer between subaccounts.
 */
int32_t lxvault_transfer(lx_t* dex, const lx_account_t* from, const lx_account_t* to,
                         const lx_currency_t* token,
                         int64_t amount_hi, uint64_t amount_lo);

/**
 * Get balance for token.
 * @param out Output balance value
 * @return true if account exists
 */
bool lxvault_get_balance(const lx_t* dex, const lx_account_t* account,
                         const lx_currency_t* token, lx_i128_t* out);

/**
 * Get margin info for account.
 */
lx_margin_info_t lxvault_get_margin(const lx_t* dex, const lx_account_t* account);

/**
 * Get position for market.
 * @param out Output position
 * @return true if position exists
 */
bool lxvault_get_position(const lx_t* dex, const lx_account_t* account,
                          uint32_t market_id, lx_position_t* out);

/**
 * Set margin mode for market.
 */
int32_t lxvault_set_margin_mode(lx_t* dex, const lx_account_t* account,
                                uint32_t market_id, lx_margin_mode_t mode);

/**
 * Add margin to isolated position.
 */
int32_t lxvault_add_margin(lx_t* dex, const lx_account_t* account,
                           uint32_t market_id, lx_i128_t amount_x18);

/**
 * Remove margin from isolated position.
 */
int32_t lxvault_remove_margin(lx_t* dex, const lx_account_t* account,
                              uint32_t market_id, lx_i128_t amount_x18);

/**
 * Check if account is liquidatable.
 */
bool lxvault_is_liquidatable(const lx_t* dex, const lx_account_t* account);

/**
 * Liquidate a position.
 */
lx_liquidation_result_t lxvault_liquidate(lx_t* dex,
                                           const lx_account_t* liquidator,
                                           const lx_account_t* account,
                                           uint32_t market_id, lx_i128_t size_x18);

/**
 * Run auto-deleverage for market.
 */
int32_t lxvault_run_adl(lx_t* dex, uint32_t market_id);

/**
 * Accrue funding for all positions in market.
 */
int32_t lxvault_accrue_funding(lx_t* dex, uint32_t market_id);

/**
 * Get insurance fund balance.
 */
lx_i128_t lxvault_insurance_balance(const lx_t* dex);

/* =============================================================================
 * LXOracle API (LP-9011) - Price Aggregation
 * ============================================================================= */

/**
 * Register an asset for price tracking.
 * @param asset_id Unique asset identifier
 * @param max_staleness Maximum price age in seconds
 * @return LX_OK on success
 */
int32_t lxoracle_register_asset(lx_t* dex, uint64_t asset_id,
                                const lx_currency_t* base_token,
                                const lx_currency_t* quote_token,
                                uint64_t max_staleness);

/**
 * Update price from a source.
 * @param price_hi High 64 bits of price (X18)
 * @param price_lo Low 64 bits of price
 * @return LX_OK on success
 */
int32_t lxoracle_update_price(lx_t* dex, uint64_t asset_id, lx_price_source_t source,
                              int64_t price_hi, uint64_t price_lo);

/**
 * Update price with confidence interval.
 */
int32_t lxoracle_update_price_with_confidence(lx_t* dex, uint64_t asset_id,
                                               lx_price_source_t source,
                                               int64_t price_hi, uint64_t price_lo,
                                               int64_t confidence_hi, uint64_t confidence_lo);

/**
 * Get aggregated price.
 * @param price_hi Output high 64 bits
 * @param price_lo Output low 64 bits
 * @return true if price available and fresh
 */
bool lxoracle_get_price(const lx_t* dex, uint64_t asset_id,
                        int64_t* price_hi, uint64_t* price_lo);

/**
 * Get price from specific source.
 */
bool lxoracle_get_source_price(const lx_t* dex, uint64_t asset_id,
                                lx_price_source_t source,
                                int64_t* price_hi, uint64_t* price_lo);

/**
 * Get TWAP over window.
 * @param window_seconds TWAP window in seconds
 */
bool lxoracle_get_twap(const lx_t* dex, uint64_t asset_id, uint64_t window_seconds,
                       int64_t* price_hi, uint64_t* price_lo);

/**
 * Check if price is fresh.
 */
bool lxoracle_is_price_fresh(const lx_t* dex, uint64_t asset_id);

/**
 * Get price age in seconds.
 */
uint64_t lxoracle_price_age(const lx_t* dex, uint64_t asset_id);

/* =============================================================================
 * LXFeed API (LP-9040) - Computed Price Feeds
 * ============================================================================= */

/**
 * Register market for price feeds.
 * @param market_id Market identifier
 * @param asset_id Oracle asset ID
 */
int32_t lxfeed_register_market(lx_t* dex, uint32_t market_id, uint64_t asset_id);

/**
 * Get mark price for market.
 */
lx_mark_price_t lxfeed_get_mark_price(const lx_t* dex, uint32_t market_id);

/**
 * Get index price for market.
 * @param price_hi Output high 64 bits
 * @param price_lo Output low 64 bits
 * @return true if available
 */
bool lxfeed_get_index_price(const lx_t* dex, uint32_t market_id,
                            int64_t* price_hi, uint64_t* price_lo);

/**
 * Get last trade price for market.
 */
bool lxfeed_get_last_price(const lx_t* dex, uint32_t market_id,
                           int64_t* price_hi, uint64_t* price_lo);

/**
 * Get mid price for market.
 */
bool lxfeed_get_mid_price(const lx_t* dex, uint32_t market_id,
                          int64_t* price_hi, uint64_t* price_lo);

/**
 * Update last trade price.
 */
void lxfeed_update_last_price(lx_t* dex, uint32_t market_id,
                              int64_t price_hi, uint64_t price_lo);

/**
 * Update BBO (best bid/offer).
 */
void lxfeed_update_bbo(lx_t* dex, uint32_t market_id,
                       int64_t best_bid_hi, uint64_t best_bid_lo,
                       int64_t best_ask_hi, uint64_t best_ask_lo);

/**
 * Get funding rate for market.
 */
lx_funding_rate_t lxfeed_get_funding_rate(const lx_t* dex, uint32_t market_id);

/**
 * Get predicted funding rate at next interval.
 */
bool lxfeed_get_predicted_funding(const lx_t* dex, uint32_t market_id,
                                   int64_t* rate_hi, uint64_t* rate_lo);

/**
 * Calculate and update funding rate.
 */
void lxfeed_calculate_funding(lx_t* dex, uint32_t market_id);

/**
 * Get premium (mark - index).
 */
bool lxfeed_get_premium(const lx_t* dex, uint32_t market_id,
                        int64_t* premium_hi, uint64_t* premium_lo);

/**
 * Get basis percentage ((mark - index) / index).
 */
bool lxfeed_get_basis(const lx_t* dex, uint32_t market_id,
                      int64_t* basis_hi, uint64_t* basis_lo);

/* =============================================================================
 * Unified Trading Interface
 * ============================================================================= */

/**
 * Create a spot market (AMM pool).
 */
int32_t lx_create_spot_market(lx_t* dex, const lx_pool_key_t* key,
                               lx_i128_t sqrt_price_x96);

/**
 * Create a perpetual market (CLOB + Vault).
 */
int32_t lx_create_perp_market(lx_t* dex, uint32_t market_id, uint64_t asset_id,
                               const lx_vault_market_config_t* vault_config,
                               const lx_book_market_config_t* book_config);

/**
 * Smart swap: routes to best venue (AMM vs CLOB).
 */
lx_balance_delta_t lx_swap_smart(lx_t* dex, const lx_account_t* sender,
                                  const lx_currency_t* token_in,
                                  const lx_currency_t* token_out,
                                  lx_i128_t amount_in_x18,
                                  lx_i128_t min_amount_out_x18);

/**
 * Update funding for a market.
 */
int32_t lx_update_funding(lx_t* dex, uint32_t market_id);

/**
 * Run liquidation checks for all accounts in market.
 */
int32_t lx_run_liquidations(lx_t* dex, uint32_t market_id);

/* =============================================================================
 * Statistics
 * ============================================================================= */

typedef struct {
    uint64_t total_pools;
    uint64_t total_swaps;
    uint64_t total_liquidity_ops;
} lx_pool_stats_t;

typedef struct {
    uint64_t total_markets;
    uint64_t total_orders_placed;
    uint64_t total_orders_cancelled;
    uint64_t total_orders_filled;
    uint64_t total_trades;
} lx_book_stats_t;

typedef struct {
    uint64_t total_accounts;
    uint64_t total_positions;
    uint64_t total_liquidations;
} lx_vault_stats_t;

typedef struct {
    uint64_t total_assets;
    uint64_t total_updates;
    uint64_t stale_prices;
} lx_oracle_stats_t;

typedef struct {
    uint64_t total_markets;
    uint64_t total_price_updates;
    uint64_t funding_calculations;
} lx_feed_stats_t;

typedef struct {
    lx_pool_stats_t pool;
    lx_book_stats_t book;
    lx_vault_stats_t vault;
    lx_oracle_stats_t oracle;
    lx_feed_stats_t feed;
    uint64_t uptime_seconds;
} lx_global_stats_t;

/**
 * Get global DEX statistics.
 */
lx_global_stats_t lx_get_stats(const lx_t* dex);

/**
 * Get pool statistics.
 */
lx_pool_stats_t lxpool_get_stats(const lx_t* dex);

/**
 * Get book statistics.
 */
lx_book_stats_t lxbook_get_stats(const lx_t* dex);

/**
 * Get vault statistics.
 */
lx_vault_stats_t lxvault_get_stats(const lx_t* dex);

/**
 * Get oracle statistics.
 */
lx_oracle_stats_t lxoracle_get_stats(const lx_t* dex);

/**
 * Get feed statistics.
 */
lx_feed_stats_t lxfeed_get_stats(const lx_t* dex);

#ifdef __cplusplus
}
#endif

#endif /* LX_C_H */
