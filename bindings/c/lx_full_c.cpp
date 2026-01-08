/**
 * lx_c.cpp - C Bindings Implementation for Full LX Stack
 */

#include "lx_c.h"
#include "lux/lx.hpp"
#include <cstring>
#include <chrono>
#include <new>

/* =============================================================================
 * 128-bit Integer Conversion Helpers
 * ============================================================================= */

static inline lux::I128 to_cpp_i128(lx_i128_t v) {
    // Reconstruct 128-bit from hi/lo
    return (static_cast<lux::I128>(v.hi) << 64) | static_cast<lux::I128>(v.lo);
}

static inline lx_i128_t to_c_i128(lux::I128 v) {
    lx_i128_t r;
    r.hi = static_cast<int64_t>(v >> 64);
    r.lo = static_cast<uint64_t>(v);
    return r;
}

static inline lux::I128 to_cpp_i128_parts(int64_t hi, uint64_t lo) {
    return (static_cast<lux::I128>(hi) << 64) | static_cast<lux::I128>(lo);
}

/* =============================================================================
 * Address Conversion
 * ============================================================================= */

static inline lux::Address to_cpp_address(const lx_address_t* addr) {
    lux::Address a;
    if (addr) {
        std::memcpy(a.data(), addr->bytes, 20);
    }
    return a;
}

static inline lx_address_t to_c_address(const lux::Address& addr) {
    lx_address_t a;
    std::memcpy(a.bytes, addr.data(), 20);
    return a;
}

/* =============================================================================
 * Currency Conversion
 * ============================================================================= */

static inline lux::Currency to_cpp_currency(const lx_currency_t* cur) {
    if (!cur) return lux::Currency();
    return lux::Currency(to_cpp_address(&cur->addr));
}

static inline lx_currency_t to_c_currency(const lux::Currency& cur) {
    lx_currency_t c;
    c.addr = to_c_address(cur.addr);
    return c;
}

/* =============================================================================
 * Account Conversion
 * ============================================================================= */

static inline lux::LXAccount to_cpp_account(const lx_account_t* acc) {
    lux::LXAccount a;
    if (acc) {
        a.main = to_cpp_address(&acc->main);
        a.subaccount_id = acc->subaccount_id;
    }
    return a;
}

static inline lx_account_t to_c_account(const lux::LXAccount& acc) {
    lx_account_t a;
    a.main = to_c_address(acc.main);
    a.subaccount_id = acc.subaccount_id;
    return a;
}

/* =============================================================================
 * Pool Key Conversion
 * ============================================================================= */

static inline lux::PoolKey to_cpp_pool_key(const lx_pool_key_t* key) {
    lux::PoolKey k;
    if (key) {
        k.currency0 = to_cpp_currency(&key->currency0);
        k.currency1 = to_cpp_currency(&key->currency1);
        k.fee = key->fee;
        k.tick_spacing = key->tick_spacing;
        k.hooks = to_cpp_address(&key->hooks);
    }
    return k;
}

/* =============================================================================
 * Swap Params Conversion
 * ============================================================================= */

static inline lux::SwapParams to_cpp_swap_params(const lx_swap_params_t* params) {
    lux::SwapParams p;
    if (params) {
        p.zero_for_one = params->zero_for_one;
        p.amount_specified = to_cpp_i128(params->amount_specified);
        p.sqrt_price_limit = to_cpp_i128(params->sqrt_price_limit);
    }
    return p;
}

/* =============================================================================
 * Modify Liquidity Params Conversion
 * ============================================================================= */

static inline lux::ModifyLiquidityParams to_cpp_modify_params(const lx_modify_params_t* params) {
    lux::ModifyLiquidityParams p;
    if (params) {
        p.tick_lower = params->tick_lower;
        p.tick_upper = params->tick_upper;
        p.liquidity_delta = to_cpp_i128(params->liquidity_delta);
        p.salt = params->salt;
    }
    return p;
}

/* =============================================================================
 * Balance Delta Conversion
 * ============================================================================= */

static inline lx_balance_delta_t to_c_balance_delta(const lux::BalanceDelta& delta) {
    lx_balance_delta_t d;
    d.amount0 = to_c_i128(delta.amount0);
    d.amount1 = to_c_i128(delta.amount1);
    return d;
}

/* =============================================================================
 * Slot0 Conversion
 * ============================================================================= */

static inline lx_slot0_t to_c_slot0(const lux::Slot0& slot) {
    lx_slot0_t s;
    s.sqrt_price_x96 = to_c_i128(slot.sqrt_price_x96);
    s.tick = slot.tick;
    s.protocol_fee = slot.protocol_fee;
    s.lp_fee = slot.lp_fee;
    s.unlocked = slot.unlocked;
    return s;
}

/* =============================================================================
 * Book Market Config Conversion
 * ============================================================================= */

static inline lux::BookMarketConfig to_cpp_book_config(const lx_book_market_config_t* cfg) {
    lux::BookMarketConfig c;
    if (cfg) {
        c.market_id = cfg->market_id;
        c.symbol_id = cfg->symbol_id;
        c.base_currency = to_cpp_currency(&cfg->base_currency);
        c.quote_currency = to_cpp_currency(&cfg->quote_currency);
        c.tick_size_x18 = to_cpp_i128(cfg->tick_size_x18);
        c.lot_size_x18 = to_cpp_i128(cfg->lot_size_x18);
        c.min_notional_x18 = to_cpp_i128(cfg->min_notional_x18);
        c.max_order_size_x18 = to_cpp_i128(cfg->max_order_size_x18);
        c.post_only_mode = cfg->post_only_mode;
        c.reduce_only_mode = cfg->reduce_only_mode;
        c.status = cfg->status;
    }
    return c;
}

/* =============================================================================
 * Vault Market Config Conversion
 * ============================================================================= */

static inline lux::MarketConfig to_cpp_vault_config(const lx_vault_market_config_t* cfg) {
    lux::MarketConfig c;
    if (cfg) {
        c.market_id = cfg->market_id;
        c.base_currency = to_cpp_currency(&cfg->base_currency);
        c.quote_currency = to_cpp_currency(&cfg->quote_currency);
        c.initial_margin_x18 = to_cpp_i128(cfg->initial_margin_x18);
        c.maintenance_margin_x18 = to_cpp_i128(cfg->maintenance_margin_x18);
        c.max_leverage_x18 = to_cpp_i128(cfg->max_leverage_x18);
        c.taker_fee_x18 = to_cpp_i128(cfg->taker_fee_x18);
        c.maker_fee_x18 = to_cpp_i128(cfg->maker_fee_x18);
        c.min_order_size_x18 = to_cpp_i128(cfg->min_order_size_x18);
        c.max_position_size_x18 = to_cpp_i128(cfg->max_position_size_x18);
        c.reduce_only_mode = cfg->reduce_only_mode;
        c.active = cfg->active;
    }
    return c;
}

/* =============================================================================
 * Order Conversion
 * ============================================================================= */

static inline lux::LXOrder to_cpp_order(const lx_order_t* order) {
    lux::LXOrder o;
    if (order) {
        o.market_id = order->market_id;
        o.is_buy = order->is_buy;
        o.kind = static_cast<lux::OrderKind>(order->kind);
        o.size_x18 = to_cpp_i128(order->size_x18);
        o.limit_px_x18 = to_cpp_i128(order->limit_px_x18);
        o.trigger_px_x18 = to_cpp_i128(order->trigger_px_x18);
        o.reduce_only = order->reduce_only;
        o.tif = static_cast<lux::TIF>(order->tif);
        std::memcpy(o.cloid.data(), order->cloid, 16);
        std::memcpy(o.group_id.data(), order->group_id, 16);
        o.group_type = static_cast<lux::GroupType>(order->group_type);
    }
    return o;
}

/* =============================================================================
 * Place Result Conversion
 * ============================================================================= */

static inline lx_place_result_t to_c_place_result(const lux::LXPlaceResult& r) {
    lx_place_result_t pr;
    pr.oid = r.oid;
    pr.status = r.status;
    pr.filled_size_x18 = to_c_i128(r.filled_size_x18);
    pr.avg_px_x18 = to_c_i128(r.avg_px_x18);
    return pr;
}

/* =============================================================================
 * L1 Conversion
 * ============================================================================= */

static inline lx_l1_t to_c_l1(const lux::LXL1& l1) {
    lx_l1_t r;
    r.best_bid_px_x18 = to_c_i128(l1.best_bid_px_x18);
    r.best_bid_sz_x18 = to_c_i128(l1.best_bid_sz_x18);
    r.best_ask_px_x18 = to_c_i128(l1.best_ask_px_x18);
    r.best_ask_sz_x18 = to_c_i128(l1.best_ask_sz_x18);
    r.last_trade_px_x18 = to_c_i128(l1.last_trade_px_x18);
    return r;
}

/* =============================================================================
 * Position Conversion
 * ============================================================================= */

static inline lx_position_t to_c_position(const lux::LXPosition& pos) {
    lx_position_t p;
    p.market_id = pos.market_id;
    p.side = static_cast<lx_position_side_t>(pos.side);
    p.size_x18 = to_c_i128(pos.size_x18);
    p.entry_px_x18 = to_c_i128(pos.entry_px_x18);
    p.unrealized_pnl_x18 = to_c_i128(pos.unrealized_pnl_x18);
    p.accumulated_funding_x18 = to_c_i128(pos.accumulated_funding_x18);
    p.last_funding_time = pos.last_funding_time;
    return p;
}

/* =============================================================================
 * Margin Info Conversion
 * ============================================================================= */

static inline lx_margin_info_t to_c_margin_info(const lux::LXMarginInfo& info) {
    lx_margin_info_t m;
    m.total_collateral_x18 = to_c_i128(info.total_collateral_x18);
    m.used_margin_x18 = to_c_i128(info.used_margin_x18);
    m.free_margin_x18 = to_c_i128(info.free_margin_x18);
    m.margin_ratio_x18 = to_c_i128(info.margin_ratio_x18);
    m.maintenance_margin_x18 = to_c_i128(info.maintenance_margin_x18);
    m.liquidatable = info.liquidatable;
    return m;
}

/* =============================================================================
 * Mark Price Conversion
 * ============================================================================= */

static inline lx_mark_price_t to_c_mark_price(const lux::LXMarkPrice& mp) {
    lx_mark_price_t m;
    m.index_px_x18 = to_c_i128(mp.index_px_x18);
    m.mark_px_x18 = to_c_i128(mp.mark_px_x18);
    m.premium_x18 = to_c_i128(mp.premium_x18);
    m.timestamp = mp.timestamp;
    return m;
}

/* =============================================================================
 * Funding Rate Conversion
 * ============================================================================= */

static inline lx_funding_rate_t to_c_funding_rate(const lux::LXFundingRate& fr) {
    lx_funding_rate_t f;
    f.rate_x18 = to_c_i128(fr.rate_x18);
    f.next_funding_time = fr.next_funding_time;
    return f;
}

/* =============================================================================
 * Liquidation Result Conversion
 * ============================================================================= */

static inline lx_liquidation_result_t to_c_liquidation_result(const lux::LXLiquidationResult& lr) {
    lx_liquidation_result_t r;
    r.liquidated = to_c_account(lr.liquidated);
    r.liquidator = to_c_account(lr.liquidator);
    r.market_id = lr.market_id;
    r.size_x18 = to_c_i128(lr.size_x18);
    r.price_x18 = to_c_i128(lr.price_x18);
    r.penalty_x18 = to_c_i128(lr.penalty_x18);
    r.adl_triggered = lr.adl_triggered;
    return r;
}

/* =============================================================================
 * C API Implementation
 * ============================================================================= */

extern "C" {

/* =============================================================================
 * LX Controller API
 * ============================================================================= */

lx_t* lx_create(void) {
    try {
        return reinterpret_cast<lx_t*>(new lux::LX());
    } catch (...) {
        return nullptr;
    }
}

lx_t* lx_create_with_config(const lx_dex_config_t* config) {
    if (!config) return lx_create();

    try {
        auto* dex = new lux::LX();
        lux::LX::Config cfg;
        cfg.engine_config.worker_threads = config->worker_threads;
        cfg.engine_config.max_batch_size = config->max_batch_size;
        cfg.enable_hooks = config->enable_hooks;
        cfg.enable_flash_loans = config->enable_flash_loans;
        cfg.funding_interval = config->funding_interval;
        cfg.default_maker_fee_x18 = to_cpp_i128(config->default_maker_fee_x18);
        cfg.default_taker_fee_x18 = to_cpp_i128(config->default_taker_fee_x18);
        dex->initialize(cfg);
        return reinterpret_cast<lx_t*>(dex);
    } catch (...) {
        return nullptr;
    }
}

void lx_destroy(lx_t* dex) {
    delete reinterpret_cast<lux::LX*>(dex);
}

int32_t lx_initialize(lx_t* dex) {
    if (!dex) return LX_ERR_NULL_POINTER;
    try {
        reinterpret_cast<lux::LX*>(dex)->initialize();
        return LX_OK;
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lx_start(lx_t* dex) {
    if (!dex) return LX_ERR_NULL_POINTER;
    try {
        reinterpret_cast<lux::LX*>(dex)->start();
        return LX_OK;
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lx_stop(lx_t* dex) {
    if (!dex) return LX_ERR_NULL_POINTER;
    try {
        reinterpret_cast<lux::LX*>(dex)->stop();
        return LX_OK;
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

bool lx_is_running(const lx_t* dex) {
    if (!dex) return false;
    return reinterpret_cast<const lux::LX*>(dex)->is_running();
}

const char* lx_version(void) {
    return lux::LX::version();
}

/* =============================================================================
 * LXPool API (LP-9010)
 * ============================================================================= */

int32_t lxpool_initialize(lx_t* dex, const lx_pool_key_t* key,
                          int64_t sqrt_price_x96_hi, uint64_t sqrt_price_x96_lo) {
    if (!dex || !key) return LX_ERR_NULL_POINTER;
    try {
        auto k = to_cpp_pool_key(key);
        auto sqrt_price = to_cpp_i128_parts(sqrt_price_x96_hi, sqrt_price_x96_lo);
        return reinterpret_cast<lux::LX*>(dex)->pool().initialize(k, sqrt_price);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

lx_balance_delta_t lxpool_swap(lx_t* dex, const lx_pool_key_t* key,
                               const lx_swap_params_t* params) {
    lx_balance_delta_t zero = {};
    if (!dex || !key || !params) return zero;

    try {
        auto k = to_cpp_pool_key(key);
        auto p = to_cpp_swap_params(params);
        auto delta = reinterpret_cast<lux::LX*>(dex)->pool().swap(k, p);
        return to_c_balance_delta(delta);
    } catch (...) {
        return zero;
    }
}

lx_balance_delta_t lxpool_modify_liquidity(lx_t* dex, const lx_pool_key_t* key,
                                           const lx_modify_params_t* params) {
    lx_balance_delta_t zero = {};
    if (!dex || !key || !params) return zero;

    try {
        auto k = to_cpp_pool_key(key);
        auto p = to_cpp_modify_params(params);
        auto delta = reinterpret_cast<lux::LX*>(dex)->pool().modify_liquidity(k, p);
        return to_c_balance_delta(delta);
    } catch (...) {
        return zero;
    }
}

lx_balance_delta_t lxpool_donate(lx_t* dex, const lx_pool_key_t* key,
                                  lx_i128_t amount0, lx_i128_t amount1) {
    lx_balance_delta_t zero = {};
    if (!dex || !key) return zero;

    try {
        auto k = to_cpp_pool_key(key);
        auto delta = reinterpret_cast<lux::LX*>(dex)->pool().donate(
            k, to_cpp_i128(amount0), to_cpp_i128(amount1));
        return to_c_balance_delta(delta);
    } catch (...) {
        return zero;
    }
}

bool lxpool_get_slot0(const lx_t* dex, const lx_pool_key_t* key, lx_slot0_t* out) {
    if (!dex || !key || !out) return false;

    try {
        auto k = to_cpp_pool_key(key);
        auto slot = reinterpret_cast<const lux::LX*>(dex)->pool().get_slot0(k);
        if (!slot) return false;
        *out = to_c_slot0(*slot);
        return true;
    } catch (...) {
        return false;
    }
}

bool lxpool_get_liquidity(const lx_t* dex, const lx_pool_key_t* key, lx_i128_t* out) {
    if (!dex || !key || !out) return false;

    try {
        auto k = to_cpp_pool_key(key);
        auto liq = reinterpret_cast<const lux::LX*>(dex)->pool().get_liquidity(k);
        if (!liq) return false;
        *out = to_c_i128(*liq);
        return true;
    } catch (...) {
        return false;
    }
}

bool lxpool_exists(const lx_t* dex, const lx_pool_key_t* key) {
    if (!dex || !key) return false;
    try {
        auto k = to_cpp_pool_key(key);
        return reinterpret_cast<const lux::LX*>(dex)->pool().pool_exists(k);
    } catch (...) {
        return false;
    }
}

int32_t lxpool_set_protocol_fee(lx_t* dex, const lx_pool_key_t* key, uint32_t new_fee) {
    if (!dex || !key) return LX_ERR_NULL_POINTER;
    try {
        auto k = to_cpp_pool_key(key);
        reinterpret_cast<lux::LX*>(dex)->pool().set_protocol_fee(k, new_fee);
        return LX_OK;
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

lx_balance_delta_t lxpool_collect_protocol(lx_t* dex, const lx_pool_key_t* key,
                                            const lx_address_t* recipient) {
    lx_balance_delta_t zero = {};
    if (!dex || !key || !recipient) return zero;

    try {
        auto k = to_cpp_pool_key(key);
        auto addr = to_cpp_address(recipient);
        auto delta = reinterpret_cast<lux::LX*>(dex)->pool().collect_protocol(k, addr);
        return to_c_balance_delta(delta);
    } catch (...) {
        return zero;
    }
}

/* =============================================================================
 * LXBook API (LP-9020)
 * ============================================================================= */

int32_t lxbook_create_market(lx_t* dex, const lx_book_market_config_t* config) {
    if (!dex || !config) return LX_ERR_NULL_POINTER;
    try {
        auto cfg = to_cpp_book_config(config);
        return reinterpret_cast<lux::LX*>(dex)->book().create_market(cfg);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lxbook_update_market(lx_t* dex, const lx_book_market_config_t* config) {
    if (!dex || !config) return LX_ERR_NULL_POINTER;
    try {
        auto cfg = to_cpp_book_config(config);
        return reinterpret_cast<lux::LX*>(dex)->book().update_market_config(cfg);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

bool lxbook_market_exists(const lx_t* dex, uint32_t market_id) {
    if (!dex) return false;
    return reinterpret_cast<const lux::LX*>(dex)->book().market_exists(market_id);
}

uint8_t lxbook_get_market_status(const lx_t* dex, uint32_t market_id) {
    if (!dex) return 0;
    return reinterpret_cast<const lux::LX*>(dex)->book().get_market_status(market_id);
}

lx_place_result_t lxbook_place_order(lx_t* dex, const lx_account_t* sender,
                                      const lx_order_t* order) {
    lx_place_result_t zero = {};
    if (!dex || !sender || !order) return zero;

    try {
        auto acc = to_cpp_account(sender);
        auto ord = to_cpp_order(order);
        auto result = reinterpret_cast<lux::LX*>(dex)->book().place_order(acc, ord);
        return to_c_place_result(result);
    } catch (...) {
        return zero;
    }
}

int32_t lxbook_cancel_order(lx_t* dex, const lx_account_t* sender,
                            uint32_t market_id, uint64_t oid) {
    if (!dex || !sender) return LX_ERR_NULL_POINTER;
    try {
        auto acc = to_cpp_account(sender);
        return reinterpret_cast<lux::LX*>(dex)->book().cancel_order(acc, market_id, oid);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lxbook_cancel_by_cloid(lx_t* dex, const lx_account_t* sender,
                               uint32_t market_id, const uint8_t* cloid) {
    if (!dex || !sender || !cloid) return LX_ERR_NULL_POINTER;
    try {
        auto acc = to_cpp_account(sender);
        std::array<uint8_t, 16> id;
        std::memcpy(id.data(), cloid, 16);
        return reinterpret_cast<lux::LX*>(dex)->book().cancel_by_cloid(acc, market_id, id);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lxbook_cancel_all(lx_t* dex, const lx_account_t* sender, uint32_t market_id) {
    if (!dex || !sender) return LX_ERR_NULL_POINTER;
    try {
        auto acc = to_cpp_account(sender);
        return reinterpret_cast<lux::LX*>(dex)->book().cancel_all(acc, market_id);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

lx_place_result_t lxbook_amend_order(lx_t* dex, const lx_account_t* sender,
                                      uint32_t market_id, uint64_t oid,
                                      lx_i128_t new_size_x18, lx_i128_t new_price_x18) {
    lx_place_result_t zero = {};
    if (!dex || !sender) return zero;

    try {
        auto acc = to_cpp_account(sender);
        auto result = reinterpret_cast<lux::LX*>(dex)->book().amend_order(
            acc, market_id, oid,
            to_cpp_i128(new_size_x18), to_cpp_i128(new_price_x18));
        return to_c_place_result(result);
    } catch (...) {
        return zero;
    }
}

lx_l1_t lxbook_get_l1(const lx_t* dex, uint32_t market_id) {
    lx_l1_t zero = {};
    if (!dex) return zero;

    try {
        auto l1 = reinterpret_cast<const lux::LX*>(dex)->book().get_l1(market_id);
        return to_c_l1(l1);
    } catch (...) {
        return zero;
    }
}

size_t lxbook_order_count(const lx_t* dex, const lx_account_t* account,
                          uint32_t market_id) {
    if (!dex || !account) return 0;
    try {
        auto acc = to_cpp_account(account);
        auto orders = reinterpret_cast<const lux::LX*>(dex)->book().get_orders(acc, market_id);
        return orders.size();
    } catch (...) {
        return 0;
    }
}

/* =============================================================================
 * LXVault API (LP-9030)
 * ============================================================================= */

int32_t lxvault_create_market(lx_t* dex, const lx_vault_market_config_t* config) {
    if (!dex || !config) return LX_ERR_NULL_POINTER;
    try {
        auto cfg = to_cpp_vault_config(config);
        return reinterpret_cast<lux::LX*>(dex)->vault().create_market(cfg);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lxvault_update_market(lx_t* dex, const lx_vault_market_config_t* config) {
    if (!dex || !config) return LX_ERR_NULL_POINTER;
    try {
        auto cfg = to_cpp_vault_config(config);
        return reinterpret_cast<lux::LX*>(dex)->vault().update_market(cfg);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lxvault_deposit(lx_t* dex, const lx_account_t* account,
                        const lx_currency_t* token,
                        int64_t amount_hi, uint64_t amount_lo) {
    if (!dex || !account || !token) return LX_ERR_NULL_POINTER;
    try {
        auto acc = to_cpp_account(account);
        auto cur = to_cpp_currency(token);
        auto amount = to_cpp_i128_parts(amount_hi, amount_lo);
        return reinterpret_cast<lux::LX*>(dex)->vault().deposit(acc, cur, amount);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lxvault_withdraw(lx_t* dex, const lx_account_t* account,
                         const lx_currency_t* token,
                         int64_t amount_hi, uint64_t amount_lo) {
    if (!dex || !account || !token) return LX_ERR_NULL_POINTER;
    try {
        auto acc = to_cpp_account(account);
        auto cur = to_cpp_currency(token);
        auto amount = to_cpp_i128_parts(amount_hi, amount_lo);
        return reinterpret_cast<lux::LX*>(dex)->vault().withdraw(acc, cur, amount);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lxvault_transfer(lx_t* dex, const lx_account_t* from, const lx_account_t* to,
                         const lx_currency_t* token,
                         int64_t amount_hi, uint64_t amount_lo) {
    if (!dex || !from || !to || !token) return LX_ERR_NULL_POINTER;
    try {
        auto f = to_cpp_account(from);
        auto t = to_cpp_account(to);
        auto cur = to_cpp_currency(token);
        auto amount = to_cpp_i128_parts(amount_hi, amount_lo);
        return reinterpret_cast<lux::LX*>(dex)->vault().transfer(f, t, cur, amount);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

bool lxvault_get_balance(const lx_t* dex, const lx_account_t* account,
                         const lx_currency_t* token, lx_i128_t* out) {
    if (!dex || !account || !token || !out) return false;
    try {
        auto acc = to_cpp_account(account);
        auto cur = to_cpp_currency(token);
        auto balance = reinterpret_cast<const lux::LX*>(dex)->vault().get_balance(acc, cur);
        *out = to_c_i128(balance);
        return true;
    } catch (...) {
        return false;
    }
}

lx_margin_info_t lxvault_get_margin(const lx_t* dex, const lx_account_t* account) {
    lx_margin_info_t zero = {};
    if (!dex || !account) return zero;

    try {
        auto acc = to_cpp_account(account);
        auto info = reinterpret_cast<const lux::LX*>(dex)->vault().get_margin_info(acc);
        return to_c_margin_info(info);
    } catch (...) {
        return zero;
    }
}

bool lxvault_get_position(const lx_t* dex, const lx_account_t* account,
                          uint32_t market_id, lx_position_t* out) {
    if (!dex || !account || !out) return false;
    try {
        auto acc = to_cpp_account(account);
        auto pos = reinterpret_cast<const lux::LX*>(dex)->vault().get_position(acc, market_id);
        if (!pos) return false;
        *out = to_c_position(*pos);
        return true;
    } catch (...) {
        return false;
    }
}

int32_t lxvault_set_margin_mode(lx_t* dex, const lx_account_t* account,
                                uint32_t market_id, lx_margin_mode_t mode) {
    if (!dex || !account) return LX_ERR_NULL_POINTER;
    try {
        auto acc = to_cpp_account(account);
        return reinterpret_cast<lux::LX*>(dex)->vault().set_margin_mode(
            acc, market_id, static_cast<lux::MarginMode>(mode));
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lxvault_add_margin(lx_t* dex, const lx_account_t* account,
                           uint32_t market_id, lx_i128_t amount_x18) {
    if (!dex || !account) return LX_ERR_NULL_POINTER;
    try {
        auto acc = to_cpp_account(account);
        return reinterpret_cast<lux::LX*>(dex)->vault().add_margin(
            acc, market_id, to_cpp_i128(amount_x18));
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lxvault_remove_margin(lx_t* dex, const lx_account_t* account,
                              uint32_t market_id, lx_i128_t amount_x18) {
    if (!dex || !account) return LX_ERR_NULL_POINTER;
    try {
        auto acc = to_cpp_account(account);
        return reinterpret_cast<lux::LX*>(dex)->vault().remove_margin(
            acc, market_id, to_cpp_i128(amount_x18));
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

bool lxvault_is_liquidatable(const lx_t* dex, const lx_account_t* account) {
    if (!dex || !account) return false;
    try {
        auto acc = to_cpp_account(account);
        return reinterpret_cast<const lux::LX*>(dex)->vault().is_liquidatable(acc);
    } catch (...) {
        return false;
    }
}

lx_liquidation_result_t lxvault_liquidate(lx_t* dex,
                                           const lx_account_t* liquidator,
                                           const lx_account_t* account,
                                           uint32_t market_id, lx_i128_t size_x18) {
    lx_liquidation_result_t zero = {};
    if (!dex || !liquidator || !account) return zero;

    try {
        auto liq = to_cpp_account(liquidator);
        auto acc = to_cpp_account(account);
        auto result = reinterpret_cast<lux::LX*>(dex)->vault().liquidate(
            liq, acc, market_id, to_cpp_i128(size_x18));
        return to_c_liquidation_result(result);
    } catch (...) {
        return zero;
    }
}

int32_t lxvault_run_adl(lx_t* dex, uint32_t market_id) {
    if (!dex) return LX_ERR_NULL_POINTER;
    try {
        return reinterpret_cast<lux::LX*>(dex)->vault().run_adl(market_id);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lxvault_accrue_funding(lx_t* dex, uint32_t market_id) {
    if (!dex) return LX_ERR_NULL_POINTER;
    try {
        return reinterpret_cast<lux::LX*>(dex)->vault().accrue_funding(market_id);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

lx_i128_t lxvault_insurance_balance(const lx_t* dex) {
    lx_i128_t zero = {};
    if (!dex) return zero;
    try {
        auto balance = reinterpret_cast<const lux::LX*>(dex)->vault().insurance_fund_balance();
        return to_c_i128(balance);
    } catch (...) {
        return zero;
    }
}

/* =============================================================================
 * LXOracle API (LP-9011)
 * ============================================================================= */

int32_t lxoracle_register_asset(lx_t* dex, uint64_t asset_id,
                                const lx_currency_t* base_token,
                                const lx_currency_t* quote_token,
                                uint64_t max_staleness) {
    if (!dex || !base_token || !quote_token) return LX_ERR_NULL_POINTER;
    try {
        lux::OracleConfig cfg;
        cfg.asset_id = asset_id;
        cfg.base_token = to_cpp_currency(base_token);
        cfg.quote_token = to_cpp_currency(quote_token);
        cfg.max_staleness = max_staleness;
        cfg.method = lux::AggregationMethod::MEDIAN;
        return reinterpret_cast<lux::LX*>(dex)->oracle().register_asset(cfg);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lxoracle_update_price(lx_t* dex, uint64_t asset_id, lx_price_source_t source,
                              int64_t price_hi, uint64_t price_lo) {
    if (!dex) return LX_ERR_NULL_POINTER;
    try {
        auto price = to_cpp_i128_parts(price_hi, price_lo);
        return reinterpret_cast<lux::LX*>(dex)->oracle().update_price(
            asset_id, static_cast<lux::PriceSource>(source), price, 0);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lxoracle_update_price_with_confidence(lx_t* dex, uint64_t asset_id,
                                               lx_price_source_t source,
                                               int64_t price_hi, uint64_t price_lo,
                                               int64_t confidence_hi, uint64_t confidence_lo) {
    if (!dex) return LX_ERR_NULL_POINTER;
    try {
        auto price = to_cpp_i128_parts(price_hi, price_lo);
        auto conf = to_cpp_i128_parts(confidence_hi, confidence_lo);
        return reinterpret_cast<lux::LX*>(dex)->oracle().update_price(
            asset_id, static_cast<lux::PriceSource>(source), price, conf);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

bool lxoracle_get_price(const lx_t* dex, uint64_t asset_id,
                        int64_t* price_hi, uint64_t* price_lo) {
    if (!dex || !price_hi || !price_lo) return false;
    try {
        auto price = reinterpret_cast<const lux::LX*>(dex)->oracle().get_price(asset_id);
        if (!price) return false;
        auto c = to_c_i128(*price);
        *price_hi = c.hi;
        *price_lo = c.lo;
        return true;
    } catch (...) {
        return false;
    }
}

bool lxoracle_get_source_price(const lx_t* dex, uint64_t asset_id,
                                lx_price_source_t source,
                                int64_t* price_hi, uint64_t* price_lo) {
    if (!dex || !price_hi || !price_lo) return false;
    try {
        auto data = reinterpret_cast<const lux::LX*>(dex)->oracle().get_source_price(
            asset_id, static_cast<lux::PriceSource>(source));
        if (!data || !data->is_valid) return false;
        auto c = to_c_i128(data->price_x18);
        *price_hi = c.hi;
        *price_lo = c.lo;
        return true;
    } catch (...) {
        return false;
    }
}

bool lxoracle_get_twap(const lx_t* dex, uint64_t asset_id, uint64_t window_seconds,
                       int64_t* price_hi, uint64_t* price_lo) {
    if (!dex || !price_hi || !price_lo) return false;
    try {
        auto twap = reinterpret_cast<const lux::LX*>(dex)->oracle().get_twap(asset_id, window_seconds);
        if (!twap) return false;
        auto c = to_c_i128(*twap);
        *price_hi = c.hi;
        *price_lo = c.lo;
        return true;
    } catch (...) {
        return false;
    }
}

bool lxoracle_is_price_fresh(const lx_t* dex, uint64_t asset_id) {
    if (!dex) return false;
    return reinterpret_cast<const lux::LX*>(dex)->oracle().is_price_fresh(asset_id);
}

uint64_t lxoracle_price_age(const lx_t* dex, uint64_t asset_id) {
    if (!dex) return UINT64_MAX;
    return reinterpret_cast<const lux::LX*>(dex)->oracle().price_age(asset_id);
}

/* =============================================================================
 * LXFeed API (LP-9040)
 * ============================================================================= */

int32_t lxfeed_register_market(lx_t* dex, uint32_t market_id, uint64_t asset_id) {
    if (!dex) return LX_ERR_NULL_POINTER;
    try {
        return reinterpret_cast<lux::LX*>(dex)->feed().register_market(market_id, asset_id);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

lx_mark_price_t lxfeed_get_mark_price(const lx_t* dex, uint32_t market_id) {
    lx_mark_price_t zero = {};
    if (!dex) return zero;

    try {
        auto mp = reinterpret_cast<const lux::LX*>(dex)->feed().get_mark_price(market_id);
        if (!mp) return zero;
        return to_c_mark_price(*mp);
    } catch (...) {
        return zero;
    }
}

bool lxfeed_get_index_price(const lx_t* dex, uint32_t market_id,
                            int64_t* price_hi, uint64_t* price_lo) {
    if (!dex || !price_hi || !price_lo) return false;
    try {
        auto price = reinterpret_cast<const lux::LX*>(dex)->feed().index_price(market_id);
        if (!price) return false;
        auto c = to_c_i128(*price);
        *price_hi = c.hi;
        *price_lo = c.lo;
        return true;
    } catch (...) {
        return false;
    }
}

bool lxfeed_get_last_price(const lx_t* dex, uint32_t market_id,
                           int64_t* price_hi, uint64_t* price_lo) {
    if (!dex || !price_hi || !price_lo) return false;
    try {
        auto price = reinterpret_cast<const lux::LX*>(dex)->feed().last_price(market_id);
        if (!price) return false;
        auto c = to_c_i128(*price);
        *price_hi = c.hi;
        *price_lo = c.lo;
        return true;
    } catch (...) {
        return false;
    }
}

bool lxfeed_get_mid_price(const lx_t* dex, uint32_t market_id,
                          int64_t* price_hi, uint64_t* price_lo) {
    if (!dex || !price_hi || !price_lo) return false;
    try {
        auto price = reinterpret_cast<const lux::LX*>(dex)->feed().mid_price(market_id);
        if (!price) return false;
        auto c = to_c_i128(*price);
        *price_hi = c.hi;
        *price_lo = c.lo;
        return true;
    } catch (...) {
        return false;
    }
}

void lxfeed_update_last_price(lx_t* dex, uint32_t market_id,
                              int64_t price_hi, uint64_t price_lo) {
    if (!dex) return;
    try {
        auto price = to_cpp_i128_parts(price_hi, price_lo);
        reinterpret_cast<lux::LX*>(dex)->feed().update_last_price(market_id, price);
    } catch (...) {}
}

void lxfeed_update_bbo(lx_t* dex, uint32_t market_id,
                       int64_t best_bid_hi, uint64_t best_bid_lo,
                       int64_t best_ask_hi, uint64_t best_ask_lo) {
    if (!dex) return;
    try {
        auto bid = to_cpp_i128_parts(best_bid_hi, best_bid_lo);
        auto ask = to_cpp_i128_parts(best_ask_hi, best_ask_lo);
        reinterpret_cast<lux::LX*>(dex)->feed().update_bbo(market_id, bid, ask);
    } catch (...) {}
}

lx_funding_rate_t lxfeed_get_funding_rate(const lx_t* dex, uint32_t market_id) {
    lx_funding_rate_t zero = {};
    if (!dex) return zero;

    try {
        auto fr = reinterpret_cast<const lux::LX*>(dex)->feed().get_funding_rate(market_id);
        if (!fr) return zero;
        return to_c_funding_rate(*fr);
    } catch (...) {
        return zero;
    }
}

bool lxfeed_get_predicted_funding(const lx_t* dex, uint32_t market_id,
                                   int64_t* rate_hi, uint64_t* rate_lo) {
    if (!dex || !rate_hi || !rate_lo) return false;
    try {
        auto rate = reinterpret_cast<const lux::LX*>(dex)->feed().predicted_funding_rate(market_id);
        if (!rate) return false;
        auto c = to_c_i128(*rate);
        *rate_hi = c.hi;
        *rate_lo = c.lo;
        return true;
    } catch (...) {
        return false;
    }
}

void lxfeed_calculate_funding(lx_t* dex, uint32_t market_id) {
    if (!dex) return;
    try {
        reinterpret_cast<lux::LX*>(dex)->feed().calculate_funding_rate(market_id);
    } catch (...) {}
}

bool lxfeed_get_premium(const lx_t* dex, uint32_t market_id,
                        int64_t* premium_hi, uint64_t* premium_lo) {
    if (!dex || !premium_hi || !premium_lo) return false;
    try {
        auto prem = reinterpret_cast<const lux::LX*>(dex)->feed().premium(market_id);
        if (!prem) return false;
        auto c = to_c_i128(*prem);
        *premium_hi = c.hi;
        *premium_lo = c.lo;
        return true;
    } catch (...) {
        return false;
    }
}

bool lxfeed_get_basis(const lx_t* dex, uint32_t market_id,
                      int64_t* basis_hi, uint64_t* basis_lo) {
    if (!dex || !basis_hi || !basis_lo) return false;
    try {
        auto bas = reinterpret_cast<const lux::LX*>(dex)->feed().basis(market_id);
        if (!bas) return false;
        auto c = to_c_i128(*bas);
        *basis_hi = c.hi;
        *basis_lo = c.lo;
        return true;
    } catch (...) {
        return false;
    }
}

/* =============================================================================
 * Unified Trading Interface
 * ============================================================================= */

int32_t lx_create_spot_market(lx_t* dex, const lx_pool_key_t* key,
                               lx_i128_t sqrt_price_x96) {
    if (!dex || !key) return LX_ERR_NULL_POINTER;
    try {
        auto k = to_cpp_pool_key(key);
        return reinterpret_cast<lux::LX*>(dex)->create_spot_market(k, to_cpp_i128(sqrt_price_x96));
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lx_create_perp_market(lx_t* dex, uint32_t market_id, uint64_t asset_id,
                               const lx_vault_market_config_t* vault_config,
                               const lx_book_market_config_t* book_config) {
    if (!dex || !vault_config || !book_config) return LX_ERR_NULL_POINTER;
    try {
        auto vcfg = to_cpp_vault_config(vault_config);
        auto bcfg = to_cpp_book_config(book_config);
        return reinterpret_cast<lux::LX*>(dex)->create_perp_market(market_id, asset_id, vcfg, bcfg);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

lx_balance_delta_t lx_swap_smart(lx_t* dex, const lx_account_t* sender,
                                  const lx_currency_t* token_in,
                                  const lx_currency_t* token_out,
                                  lx_i128_t amount_in_x18,
                                  lx_i128_t min_amount_out_x18) {
    lx_balance_delta_t zero = {};
    if (!dex || !sender || !token_in || !token_out) return zero;

    try {
        auto acc = to_cpp_account(sender);
        auto in = to_cpp_currency(token_in);
        auto out = to_cpp_currency(token_out);
        auto delta = reinterpret_cast<lux::LX*>(dex)->swap_smart(
            acc, in, out, to_cpp_i128(amount_in_x18), to_cpp_i128(min_amount_out_x18));
        return to_c_balance_delta(delta);
    } catch (...) {
        return zero;
    }
}

int32_t lx_update_funding(lx_t* dex, uint32_t market_id) {
    if (!dex) return LX_ERR_NULL_POINTER;
    try {
        return reinterpret_cast<lux::LX*>(dex)->update_funding(market_id);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

int32_t lx_run_liquidations(lx_t* dex, uint32_t market_id) {
    if (!dex) return LX_ERR_NULL_POINTER;
    try {
        return reinterpret_cast<lux::LX*>(dex)->run_liquidations(market_id);
    } catch (...) {
        return LX_ERR_INTERNAL;
    }
}

/* =============================================================================
 * Statistics
 * ============================================================================= */

lx_global_stats_t lx_get_stats(const lx_t* dex) {
    lx_global_stats_t zero = {};
    if (!dex) return zero;

    try {
        auto stats = reinterpret_cast<const lux::LX*>(dex)->get_stats();

        lx_global_stats_t r;
        r.pool.total_pools = stats.pool_stats.total_pools;
        r.pool.total_swaps = stats.pool_stats.total_swaps;
        r.pool.total_liquidity_ops = stats.pool_stats.total_liquidity_ops;

        r.book.total_markets = stats.book_stats.total_markets;
        r.book.total_orders_placed = stats.book_stats.total_orders_placed;
        r.book.total_orders_cancelled = stats.book_stats.total_orders_cancelled;
        r.book.total_orders_filled = stats.book_stats.total_orders_filled;
        r.book.total_trades = stats.book_stats.total_trades;

        r.vault.total_accounts = stats.vault_stats.total_accounts;
        r.vault.total_positions = stats.vault_stats.total_positions;
        r.vault.total_liquidations = stats.vault_stats.total_liquidations;

        r.oracle.total_assets = stats.oracle_stats.total_assets;
        r.oracle.total_updates = stats.oracle_stats.total_updates;
        r.oracle.stale_prices = stats.oracle_stats.stale_prices;

        r.feed.total_markets = stats.feed_stats.total_markets;
        r.feed.total_price_updates = stats.feed_stats.total_price_updates;
        r.feed.funding_calculations = stats.feed_stats.funding_calculations;

        r.uptime_seconds = stats.uptime_seconds;

        return r;
    } catch (...) {
        return zero;
    }
}

lx_pool_stats_t lxpool_get_stats(const lx_t* dex) {
    lx_pool_stats_t zero = {};
    if (!dex) return zero;

    try {
        auto stats = reinterpret_cast<const lux::LX*>(dex)->pool().get_stats();
        lx_pool_stats_t r;
        r.total_pools = stats.total_pools;
        r.total_swaps = stats.total_swaps;
        r.total_liquidity_ops = stats.total_liquidity_ops;
        return r;
    } catch (...) {
        return zero;
    }
}

lx_book_stats_t lxbook_get_stats(const lx_t* dex) {
    lx_book_stats_t zero = {};
    if (!dex) return zero;

    try {
        auto stats = reinterpret_cast<const lux::LX*>(dex)->book().get_stats();
        lx_book_stats_t r;
        r.total_markets = stats.total_markets;
        r.total_orders_placed = stats.total_orders_placed;
        r.total_orders_cancelled = stats.total_orders_cancelled;
        r.total_orders_filled = stats.total_orders_filled;
        r.total_trades = stats.total_trades;
        return r;
    } catch (...) {
        return zero;
    }
}

lx_vault_stats_t lxvault_get_stats(const lx_t* dex) {
    lx_vault_stats_t zero = {};
    if (!dex) return zero;

    try {
        auto stats = reinterpret_cast<const lux::LX*>(dex)->vault().get_stats();
        lx_vault_stats_t r;
        r.total_accounts = stats.total_accounts;
        r.total_positions = stats.total_positions;
        r.total_liquidations = stats.total_liquidations;
        return r;
    } catch (...) {
        return zero;
    }
}

lx_oracle_stats_t lxoracle_get_stats(const lx_t* dex) {
    lx_oracle_stats_t zero = {};
    if (!dex) return zero;

    try {
        auto stats = reinterpret_cast<const lux::LX*>(dex)->oracle().get_stats();
        lx_oracle_stats_t r;
        r.total_assets = stats.total_assets;
        r.total_updates = stats.total_updates;
        r.stale_prices = stats.stale_prices;
        return r;
    } catch (...) {
        return zero;
    }
}

lx_feed_stats_t lxfeed_get_stats(const lx_t* dex) {
    lx_feed_stats_t zero = {};
    if (!dex) return zero;

    try {
        auto stats = reinterpret_cast<const lux::LX*>(dex)->feed().get_stats();
        lx_feed_stats_t r;
        r.total_markets = stats.total_markets;
        r.total_price_updates = stats.total_price_updates;
        r.funding_calculations = stats.funding_calculations;
        return r;
    } catch (...) {
        return zero;
    }
}

} /* extern "C" */
