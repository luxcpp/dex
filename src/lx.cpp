// =============================================================================
// lx.cpp - Unified LX Controller
// =============================================================================

#include "lux/lx.hpp"
#include <chrono>
#include <cstring>
#include <algorithm>

namespace lux {

// =============================================================================
// ABI Encoding/Decoding Helpers
// =============================================================================

namespace abi {

// Decode uint32 from big-endian bytes
inline uint32_t decode_uint32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

// Decode uint64 from big-endian bytes
inline uint64_t decode_uint64(const uint8_t* data) {
    uint64_t result = 0;
    for (int i = 0; i < 8; ++i) {
        result = (result << 8) | data[i];
    }
    return result;
}

// Decode I128 from 32 bytes (padded, big-endian)
// EVM uses two's complement for signed integers in 256-bit slots
// Lower 128 bits are in bytes 16-31
inline I128 decode_int128(const uint8_t* data) {
    I128 result = 0;
    for (int i = 16; i < 32; ++i) {
        result = (result << 8) | data[i];
    }
    return result;
}

// Decode address from 32 bytes (padded, last 20 bytes)
inline Address decode_address(const uint8_t* data) {
    Address addr;
    std::memcpy(addr.data(), data + 12, 20);
    return addr;
}

// Encode uint32 to big-endian bytes
inline void encode_uint32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(value & 0xFF);
}

// Encode uint64 to big-endian bytes
inline void encode_uint64(uint8_t* out, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out[i] = static_cast<uint8_t>(value & 0xFF);
        value >>= 8;
    }
}

// Encode I128 to 32 bytes (padded, big-endian)
inline void encode_int128(uint8_t* out, I128 value) {
    std::memset(out, (value < 0) ? 0xFF : 0, 16);
    for (int i = 31; i >= 16; --i) {
        out[i] = static_cast<uint8_t>(value & 0xFF);
        value >>= 8;
    }
}

// Encode I128 as 32-byte vector
inline std::vector<uint8_t> encode_int128_vec(I128 value) {
    std::vector<uint8_t> result(32, (value < 0) ? 0xFF : 0);
    for (int i = 31; i >= 16; --i) {
        result[i] = static_cast<uint8_t>(value & 0xFF);
        value >>= 8;
    }
    return result;
}

// Encode success (1) or failure (0)
inline std::vector<uint8_t> encode_bool(bool value) {
    std::vector<uint8_t> result(32, 0);
    result[31] = value ? 1 : 0;
    return result;
}

// Encode int32 error code
inline std::vector<uint8_t> encode_int32(int32_t value) {
    std::vector<uint8_t> result(32, (value < 0) ? 0xFF : 0);
    result[28] = static_cast<uint8_t>((value >> 24) & 0xFF);
    result[29] = static_cast<uint8_t>((value >> 16) & 0xFF);
    result[30] = static_cast<uint8_t>((value >> 8) & 0xFF);
    result[31] = static_cast<uint8_t>(value & 0xFF);
    return result;
}

} // namespace abi

// =============================================================================
// LX Implementation
// =============================================================================

LX::LX()
    : pool_(std::make_unique<LXPool>())
    , oracle_(std::make_unique<LXOracle>())
    , vault_(std::make_unique<LXVault>())
    , book_(std::make_unique<LXBook>())
    , feed_(std::make_unique<LXFeed>(*oracle_))
    , running_(false)
    , start_time_(0) {

    // Wire up book settlement callback to vault
    book_->set_settlement_callback([this](const std::vector<Trade>& trades) {
        return on_book_trades(trades);
    });
}

LX::~LX() {
    stop();
}

// =============================================================================
// Initialization
// =============================================================================

void LX::initialize() {
    Config default_config;
    default_config.engine_config = EngineConfig{};
    default_config.enable_hooks = true;
    default_config.enable_flash_loans = true;
    default_config.funding_interval = 28800; // 8 hours
    default_config.default_maker_fee_x18 = x18::from_double(0.0002); // 0.02%
    default_config.default_taker_fee_x18 = x18::from_double(0.0005); // 0.05%

    initialize(default_config);
}

void LX::initialize(const Config& config) {
    // Engine is already initialized in book constructor
    // Additional configuration can be applied here

    // Set default funding params for feed
    FundingParams funding;
    funding.funding_interval = config.funding_interval;
    funding.max_funding_rate_x18 = x18::from_double(0.01); // 1%
    funding.interest_rate_x18 = x18::from_double(0.0001);  // 0.01%
    funding.premium_fraction_x18 = X18_ONE;
    funding.use_twap_premium = true;

    // Configure mark price defaults
    MarkPriceConfig mark_config;
    mark_config.premium_ewma_window = 300; // 5 minutes
    mark_config.impact_notional_x18 = x18::from_double(10000); // $10k
    mark_config.max_premium_x18 = x18::from_double(0.05);  // 5%
    mark_config.min_premium_x18 = x18::from_double(-0.05); // -5%
    mark_config.use_mid_price = true;
    mark_config.cap_to_oracle = true;

    start_time_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

// =============================================================================
// Lifecycle
// =============================================================================

void LX::start() {
    if (running_.exchange(true)) {
        return; // Already running
    }

    start_time_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    // Start the matching engine
    book_->get_engine()->start();
}

void LX::stop() {
    if (!running_.exchange(false)) {
        return; // Already stopped
    }

    book_->get_engine()->stop();
}

bool LX::is_running() const {
    return running_.load(std::memory_order_relaxed);
}

// =============================================================================
// Market Creation
// =============================================================================

int32_t LX::create_spot_market(const PoolKey& key, I128 sqrt_price_x96) {
    return pool_->initialize(key, sqrt_price_x96);
}

int32_t LX::create_perp_market(uint32_t market_id, uint64_t asset_id,
                               const MarketConfig& vault_config,
                               const BookMarketConfig& book_config) {
    // Create market in vault
    int32_t vault_result = vault_->create_market(vault_config);
    if (vault_result != errors::OK) {
        return vault_result;
    }

    // Create market in book
    int32_t book_result = book_->create_market(book_config);
    if (book_result != errors::OK) {
        // Rollback vault market? For now, leave it
        return book_result;
    }

    // Register market in feed
    int32_t feed_result = feed_->register_market(market_id, asset_id);
    if (feed_result != errors::OK) {
        return feed_result;
    }

    // Set default funding params
    FundingParams funding;
    funding.funding_interval = 28800;
    funding.max_funding_rate_x18 = x18::from_double(0.01);
    funding.interest_rate_x18 = x18::from_double(0.0001);
    funding.premium_fraction_x18 = X18_ONE;
    funding.use_twap_premium = true;
    feed_->set_funding_params(market_id, funding);

    // Set default mark price config
    MarkPriceConfig mark_config;
    mark_config.premium_ewma_window = 300;
    mark_config.impact_notional_x18 = x18::from_double(10000);
    mark_config.max_premium_x18 = x18::from_double(0.05);
    mark_config.min_premium_x18 = x18::from_double(-0.05);
    mark_config.use_mid_price = true;
    mark_config.cap_to_oracle = true;
    feed_->set_mark_price_config(market_id, mark_config);

    return errors::OK;
}

// =============================================================================
// Unified Trading Interface
// =============================================================================

BalanceDelta LX::swap_smart(const LXAccount& sender, const Currency& token_in,
                            const Currency& token_out, I128 amount_in_x18,
                            I128 min_amount_out_x18) {
    // Smart order routing between AMM and CLOB

    // First, check AMM price
    PoolKey key;
    if (token_in < token_out) {
        key.currency0 = token_in;
        key.currency1 = token_out;
    } else {
        key.currency0 = token_out;
        key.currency1 = token_in;
    }
    key.fee = fees::FEE_030; // Default 0.3%
    key.tick_spacing = tick_spacings::TICK_SPACING_030;

    bool zero_for_one = token_in == key.currency0;

    // Try AMM swap
    SwapParams params;
    params.zero_for_one = zero_for_one;
    params.amount_specified = amount_in_x18;
    params.sqrt_price_limit = 0; // No limit

    if (pool_->pool_exists(key)) {
        BalanceDelta amm_result = pool_->swap(key, params);

        I128 amount_out = zero_for_one ? -amm_result.amount1 : -amm_result.amount0;

        if (amount_out >= min_amount_out_x18) {
            return amm_result;
        }
    }

    // Fall back to CLOB if available
    // Would need to map tokens to market_id
    // For now, return empty if AMM failed

    return {0, 0};
}

LX::TradeResult LX::trade(const LXAccount& sender, uint32_t market_id,
                          bool is_buy, I128 size_x18, I128 limit_price_x18) {
    TradeResult result{};

    // Check both venues and route to best execution

    // 1. Check CLOB
    bool use_clob = book_->market_exists(market_id);
    LXL1 l1{};
    if (use_clob) {
        l1 = book_->get_l1(market_id);
    }

    // 2. Check AMM
    // Would need market_id -> PoolKey mapping
    bool use_amm = false; // Simplified for now

    // Determine best venue
    if (use_clob && !use_amm) {
        // Use CLOB only
        LXOrder order;
        order.market_id = market_id;
        order.is_buy = is_buy;
        order.kind = (limit_price_x18 == 0) ? OrderKind::MARKET : OrderKind::LIMIT;
        order.size_x18 = size_x18;
        order.limit_px_x18 = limit_price_x18;
        order.tif = TIF::IOC; // Immediate or cancel for market orders
        order.reduce_only = false;

        LXPlaceResult place_result = book_->place_order(sender, order);

        result.used_clob = true;
        result.delta.amount0 = place_result.filled_size_x18;
        result.delta.amount1 = x18::mul(place_result.filled_size_x18, place_result.avg_px_x18);
        if (!is_buy) {
            result.delta.amount0 = -result.delta.amount0;
            result.delta.amount1 = -result.delta.amount1;
        }
        result.effective_price_x18 = place_result.avg_px_x18;

    } else if (use_amm && !use_clob) {
        // Use AMM only
        result.used_amm = true;
        // Would execute AMM swap here

    } else if (use_amm && use_clob) {
        // Split order based on price comparison
        // For now, prefer CLOB
        result.used_clob = true;

        LXOrder order;
        order.market_id = market_id;
        order.is_buy = is_buy;
        order.kind = OrderKind::LIMIT;
        order.size_x18 = size_x18;
        order.limit_px_x18 = limit_price_x18;
        order.tif = TIF::IOC;
        order.reduce_only = false;

        LXPlaceResult place_result = book_->place_order(sender, order);

        result.delta.amount0 = place_result.filled_size_x18;
        result.delta.amount1 = x18::mul(place_result.filled_size_x18, place_result.avg_px_x18);
        result.effective_price_x18 = place_result.avg_px_x18;
    }

    return result;
}

// =============================================================================
// Cross-Component Operations
// =============================================================================

int32_t LX::settle_trades(const std::vector<Trade>& trades) {
    return on_book_trades(trades);
}

int32_t LX::update_funding(uint32_t market_id) {
    // Update mark price from feed
    feed_->calculate_funding_rate(market_id);

    // Get funding rate and set in vault
    auto rate = feed_->funding_rate(market_id);
    if (rate) {
        vault_->set_funding_rate(market_id, *rate);
    }

    // Accrue funding in vault
    return vault_->accrue_funding(market_id);
}

int32_t LX::run_liquidations(uint32_t market_id) {
    // Get mark price for liquidation checks
    auto mark = feed_->mark_price(market_id);
    if (!mark) {
        return errors::PRICE_STALE;
    }

    // Note: In production, we'd iterate through accounts and check liquidation
    // For now, this is a stub that would be called by a keeper

    return errors::OK;
}

// =============================================================================
// Statistics
// =============================================================================

LX::GlobalStats LX::get_stats() const {
    GlobalStats stats{};

    stats.pool_stats = pool_->get_stats();
    stats.book_stats = book_->get_stats();
    stats.vault_stats = vault_->get_stats();
    stats.oracle_stats = oracle_->get_stats();
    stats.feed_stats = feed_->get_stats();

    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    stats.uptime_seconds = now - start_time_;

    return stats;
}

// =============================================================================
// Internal Settlement Callback
// =============================================================================

int32_t LX::on_book_trades(const std::vector<Trade>& trades) {
    if (trades.empty()) {
        return errors::OK;
    }

    // Convert trades to settlements
    std::vector<LXSettlement> settlements;
    settlements.reserve(trades.size());

    for (const auto& trade : trades) {
        LXSettlement settlement;

        // Convert account IDs to LXAccount
        // In production, we'd have proper account mapping
        settlement.maker.main = {};
        settlement.maker.subaccount_id = static_cast<uint16_t>(trade.seller_account_id & 0xFFFF);
        settlement.taker.main = {};
        settlement.taker.subaccount_id = static_cast<uint16_t>(trade.buyer_account_id & 0xFFFF);

        settlement.market_id = static_cast<uint32_t>(trade.symbol_id);
        settlement.taker_is_buy = (trade.aggressor_side == Side::Buy);

        // Convert from 1e8 to X18
        settlement.size_x18 = static_cast<I128>(trade.quantity) * X18_ONE / 100000000LL;
        settlement.price_x18 = static_cast<I128>(trade.price) * X18_ONE / 100000000LL;

        // Calculate fees (simplified)
        I128 notional = x18::mul(settlement.size_x18, settlement.price_x18);
        settlement.maker_fee_x18 = x18::mul(notional, x18::from_double(0.0002)); // 0.02%
        settlement.taker_fee_x18 = x18::mul(notional, x18::from_double(0.0005)); // 0.05%

        settlement.flags = (trade.aggressor_side == Side::Buy) ?
            fill_flags::TAKER : fill_flags::MAKER;

        settlements.push_back(settlement);
    }

    // Pre-check fills
    int32_t check_result = vault_->pre_check_fills(settlements);
    if (check_result != errors::OK) {
        return check_result;
    }

    // Apply fills
    return vault_->apply_fills(settlements);
}

// =============================================================================
// PrecompileRouter Implementation
// =============================================================================

PrecompileRouter::PrecompileRouter(LX& dex) : dex_(dex) {
    register_pool_handlers();
    register_book_handlers();
    register_vault_handlers();
    register_oracle_handlers();
    register_feed_handlers();
}

std::vector<uint8_t> PrecompileRouter::call(const Address& precompile,
                                             const std::vector<uint8_t>& calldata) {
    if (!is_precompile(precompile)) {
        return {};
    }

    if (calldata.size() < 4) {
        return {};
    }

    // Extract method selector (first 4 bytes)
    uint32_t selector = 0;
    selector |= static_cast<uint32_t>(calldata[0]) << 24;
    selector |= static_cast<uint32_t>(calldata[1]) << 16;
    selector |= static_cast<uint32_t>(calldata[2]) << 8;
    selector |= static_cast<uint32_t>(calldata[3]);

    // Get precompile address as LP number
    uint16_t lp_num = addresses::to_lp(precompile);
    uint64_t precompile_key = lp_num;

    auto precompile_it = handlers_.find(precompile_key);
    if (precompile_it == handlers_.end()) {
        return {};
    }

    auto handler_it = precompile_it->second.find(selector);
    if (handler_it == precompile_it->second.end()) {
        return {};
    }

    // Extract calldata without selector
    std::vector<uint8_t> args(calldata.begin() + 4, calldata.end());

    return handler_it->second(args);
}

std::vector<uint8_t> PrecompileRouter::static_call(const Address& precompile,
                                                    const std::vector<uint8_t>& calldata) const {
    // For static calls, we only allow read operations
    // The handler registration would mark which are read-only
    // For now, delegate to call (non-const cast is needed)
    return const_cast<PrecompileRouter*>(this)->call(precompile, calldata);
}

bool PrecompileRouter::is_precompile(const Address& addr) const {
    return addresses::is_dex_precompile(addr);
}

uint64_t PrecompileRouter::gas_cost(const Address& precompile,
                                     const std::vector<uint8_t>& calldata) const {
    if (!is_precompile(precompile)) {
        return 0;
    }

    if (calldata.size() < 4) {
        return gas::POOL_SWAP; // Default
    }

    // Extract method selector
    uint32_t selector = 0;
    selector |= static_cast<uint32_t>(calldata[0]) << 24;
    selector |= static_cast<uint32_t>(calldata[1]) << 16;
    selector |= static_cast<uint32_t>(calldata[2]) << 8;
    selector |= static_cast<uint32_t>(calldata[3]);

    // Map precompile address to gas costs based on method
    uint16_t lp_num = addresses::to_lp(precompile);

    switch (lp_num) {
        case 0x9010: // LX_POOL
            switch (selector) {
                case 0x7a44c8ab: return gas::POOL_INITIALIZE;  // initialize
                case 0x1a686502: return gas::POOL_SWAP;        // swap
                case 0x3a7a5b04: return gas::POOL_MODIFY_LIQUIDITY; // modifyLiquidity
                case 0x4c7a25b0: return gas::POOL_DONATE;      // donate
                default: return gas::POOL_SWAP;
            }

        case 0x9011: // LX_ORACLE
            switch (selector) {
                case 0x99cff17c: return gas::ORACLE_GET_PRICE;    // getPrice
                case 0x7d3e47c1: return gas::ORACLE_UPDATE_PRICE; // updatePrice
                default: return gas::ORACLE_GET_PRICE;
            }

        case 0x9020: // LX_BOOK
            switch (selector) {
                case 0x1a4d01d2: return gas::BOOK_EXECUTE;       // execute
                case 0x4f55d24d: return gas::BOOK_PLACE_ORDER / 3; // getL1 (read-only)
                case 0x9e281a98: return gas::BOOK_CANCEL_ORDER;  // cancelOrder
                default: return gas::BOOK_PLACE_ORDER;
            }

        case 0x9030: // LX_VAULT
            switch (selector) {
                case 0x47e7ef24: return gas::VAULT_DEPOSIT;    // deposit
                case 0xf3fef3a3: return gas::VAULT_WITHDRAW;   // withdraw
                case 0x4ab42e11: return gas::VAULT_SETTLE / 2; // getPosition (read-only)
                case 0x2e1a7d4d: return gas::VAULT_LIQUIDATE;  // liquidate
                default: return gas::VAULT_DEPOSIT;
            }

        case 0x9040: // LX_FEED
            switch (selector) {
                case 0x82a0548d: return gas::FEED_GET_MARK_PRICE;    // getMarkPrice
                case 0x8c6f037f: return gas::FEED_GET_FUNDING_RATE;  // getFundingRate
                default: return gas::FEED_GET_MARK_PRICE;
            }

        default:
            return gas::POOL_SWAP;
    }
}

// =============================================================================
// Handler Registration - LXPool (LP-9010)
// =============================================================================

void PrecompileRouter::register_pool_handlers() {
    uint64_t pool_key = addresses::to_lp(addresses::LX_POOL);

    // initialize(PoolKey,uint160) -> 0x7a44c8ab
    handlers_[pool_key][0x7a44c8ab] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        // PoolKey: currency0 (20), currency1 (20), fee (4), tickSpacing (4), hooks (20)
        // sqrt_price_x96: last 20 bytes of 32-byte slot
        if (args.size() < 128) return abi::encode_int32(errors::INVALID_CURRENCY);

        PoolKey key;
        key.currency0 = Currency(abi::decode_address(args.data()));
        key.currency1 = Currency(abi::decode_address(args.data() + 32));
        key.fee = abi::decode_uint32(args.data() + 64 + 28);
        key.tick_spacing = static_cast<int32_t>(abi::decode_uint32(args.data() + 96 + 28));

        I128 sqrt_price_x96 = abi::decode_int128(args.data() + 128);

        int32_t tick = dex_.pool().initialize(key, sqrt_price_x96);
        return abi::encode_int32(tick);
    };

    // swap(PoolKey,SwapParams,bytes) -> 0x1a686502
    handlers_[pool_key][0x1a686502] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 224) return {};

        PoolKey key;
        key.currency0 = Currency(abi::decode_address(args.data()));
        key.currency1 = Currency(abi::decode_address(args.data() + 32));
        key.fee = abi::decode_uint32(args.data() + 64 + 28);
        key.tick_spacing = static_cast<int32_t>(abi::decode_uint32(args.data() + 96 + 28));
        key.hooks = abi::decode_address(args.data() + 128);

        SwapParams params;
        params.zero_for_one = args[160 + 31] != 0;
        params.amount_specified = abi::decode_int128(args.data() + 192);
        params.sqrt_price_limit = abi::decode_int128(args.data() + 224);

        BalanceDelta delta = dex_.pool().swap(key, params);

        // Encode BalanceDelta (2 x int256)
        std::vector<uint8_t> result(64);
        abi::encode_int128(result.data(), delta.amount0);
        abi::encode_int128(result.data() + 32, delta.amount1);
        return result;
    };

    // modifyLiquidity(PoolKey,ModifyLiquidityParams,bytes) -> 0x3a7a5b04
    handlers_[pool_key][0x3a7a5b04] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 256) return {};

        PoolKey key;
        key.currency0 = Currency(abi::decode_address(args.data()));
        key.currency1 = Currency(abi::decode_address(args.data() + 32));
        key.fee = abi::decode_uint32(args.data() + 64 + 28);
        key.tick_spacing = static_cast<int32_t>(abi::decode_uint32(args.data() + 96 + 28));
        key.hooks = abi::decode_address(args.data() + 128);

        ModifyLiquidityParams params;
        params.tick_lower = static_cast<int32_t>(abi::decode_uint32(args.data() + 160 + 28));
        params.tick_upper = static_cast<int32_t>(abi::decode_uint32(args.data() + 192 + 28));
        params.liquidity_delta = abi::decode_int128(args.data() + 224);
        params.salt = abi::decode_uint64(args.data() + 256 + 24);

        BalanceDelta delta = dex_.pool().modify_liquidity(key, params);

        std::vector<uint8_t> result(64);
        abi::encode_int128(result.data(), delta.amount0);
        abi::encode_int128(result.data() + 32, delta.amount1);
        return result;
    };

    // getSlot0(PoolKey) -> 0x9e5e2e15
    handlers_[pool_key][0x9e5e2e15] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 160) return {};

        PoolKey key;
        key.currency0 = Currency(abi::decode_address(args.data()));
        key.currency1 = Currency(abi::decode_address(args.data() + 32));
        key.fee = abi::decode_uint32(args.data() + 64 + 28);
        key.tick_spacing = static_cast<int32_t>(abi::decode_uint32(args.data() + 96 + 28));
        key.hooks = abi::decode_address(args.data() + 128);

        auto slot0 = dex_.pool().get_slot0(key);
        if (!slot0) return {};

        std::vector<uint8_t> result(160);
        abi::encode_int128(result.data(), slot0->sqrt_price_x96);
        abi::encode_int128(result.data() + 32, static_cast<I128>(slot0->tick));
        abi::encode_int128(result.data() + 64, static_cast<I128>(slot0->protocol_fee));
        abi::encode_int128(result.data() + 96, static_cast<I128>(slot0->lp_fee));
        result[128 + 31] = slot0->unlocked ? 1 : 0;
        return result;
    };
}

// =============================================================================
// Handler Registration - LXBook (LP-9020)
// =============================================================================

void PrecompileRouter::register_book_handlers() {
    uint64_t book_key = addresses::to_lp(addresses::LX_BOOK);

    // execute(Action) -> 0x1a4d01d2
    handlers_[book_key][0x1a4d01d2] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 96) return {};

        // Decode LXAction
        LXAction action;
        action.action_type = static_cast<ActionType>(args[31]);
        action.nonce = abi::decode_uint64(args.data() + 32 + 24);
        action.expires_after = abi::decode_uint64(args.data() + 64 + 24);

        // Decode sender from first 20 bytes
        LXAccount sender;
        sender.main = abi::decode_address(args.data());

        // Execute action
        ExecuteResult result = dex_.book().execute(sender, action);

        // Encode result
        return abi::encode_int32(result.error_code);
    };

    // getL1(uint32) -> 0x4f55d24d
    handlers_[book_key][0x4f55d24d] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint32_t market_id = abi::decode_uint32(args.data() + 28);
        LXL1 l1 = dex_.book().get_l1(market_id);

        // Encode LXL1: 5 x int256
        std::vector<uint8_t> result(160);
        abi::encode_int128(result.data(), l1.best_bid_px_x18);
        abi::encode_int128(result.data() + 32, l1.best_bid_sz_x18);
        abi::encode_int128(result.data() + 64, l1.best_ask_px_x18);
        abi::encode_int128(result.data() + 96, l1.best_ask_sz_x18);
        abi::encode_int128(result.data() + 128, l1.last_trade_px_x18);
        return result;
    };

    // placeOrder(LXOrder) -> 0x3e5b3a12
    handlers_[book_key][0x3e5b3a12] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 256) return {};

        LXAccount sender;
        sender.main = abi::decode_address(args.data());

        LXOrder order;
        order.market_id = abi::decode_uint32(args.data() + 32 + 28);
        order.is_buy = args[64 + 31] != 0;
        order.kind = static_cast<OrderKind>(args[96 + 31]);
        order.size_x18 = abi::decode_int128(args.data() + 128);
        order.limit_px_x18 = abi::decode_int128(args.data() + 160);
        order.trigger_px_x18 = abi::decode_int128(args.data() + 192);
        order.reduce_only = args[224 + 31] != 0;
        order.tif = static_cast<TIF>(args[256 + 31]);

        LXPlaceResult result = dex_.book().place_order(sender, order);

        // Encode result: oid (uint64), status (uint8), filled_size (int128), avg_px (int128)
        std::vector<uint8_t> encoded(128);
        abi::encode_uint64(encoded.data() + 24, result.oid);
        encoded[63] = result.status;
        abi::encode_int128(encoded.data() + 64, result.filled_size_x18);
        abi::encode_int128(encoded.data() + 96, result.avg_px_x18);
        return encoded;
    };

    // cancelOrder(uint32,uint64) -> 0x9e281a98
    handlers_[book_key][0x9e281a98] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 96) return {};

        LXAccount sender;
        sender.main = abi::decode_address(args.data());

        uint32_t market_id = abi::decode_uint32(args.data() + 32 + 28);
        uint64_t oid = abi::decode_uint64(args.data() + 64 + 24);

        int32_t result = dex_.book().cancel_order(sender, market_id, oid);
        return abi::encode_int32(result);
    };

    // getOrder(uint32,uint64) -> 0x7c8d9e11
    handlers_[book_key][0x7c8d9e11] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 64) return {};

        uint32_t market_id = abi::decode_uint32(args.data() + 28);
        uint64_t oid = abi::decode_uint64(args.data() + 32 + 24);

        auto order = dex_.book().get_order(market_id, oid);
        if (!order) return {};

        // Encode BookOrderState
        std::vector<uint8_t> result(320);
        abi::encode_uint64(result.data() + 24, order->oid);
        abi::encode_int128(result.data() + 64, order->original_size_x18);
        abi::encode_int128(result.data() + 96, order->remaining_size_x18);
        abi::encode_int128(result.data() + 128, order->filled_size_x18);
        abi::encode_int128(result.data() + 160, order->limit_price_x18);
        abi::encode_int128(result.data() + 192, order->avg_fill_price_x18);
        result[224 + 31] = static_cast<uint8_t>(order->status);
        return result;
    };
}

// =============================================================================
// Handler Registration - LXVault (LP-9030)
// =============================================================================

void PrecompileRouter::register_vault_handlers() {
    uint64_t vault_key = addresses::to_lp(addresses::LX_VAULT);

    // deposit(address,uint256) -> 0x47e7ef24
    handlers_[vault_key][0x47e7ef24] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 96) return {};

        LXAccount account;
        account.main = abi::decode_address(args.data());

        Currency token;
        token.addr = abi::decode_address(args.data() + 32);

        I128 amount_x18 = abi::decode_int128(args.data() + 64);

        int32_t result = dex_.vault().deposit(account, token, amount_x18);
        return abi::encode_int32(result);
    };

    // withdraw(address,uint256) -> 0xf3fef3a3
    handlers_[vault_key][0xf3fef3a3] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 96) return {};

        LXAccount account;
        account.main = abi::decode_address(args.data());

        Currency token;
        token.addr = abi::decode_address(args.data() + 32);

        I128 amount_x18 = abi::decode_int128(args.data() + 64);

        int32_t result = dex_.vault().withdraw(account, token, amount_x18);
        return abi::encode_int32(result);
    };

    // getPosition(address,uint32) -> 0x4ab42e11
    handlers_[vault_key][0x4ab42e11] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 64) return {};

        LXAccount account;
        account.main = abi::decode_address(args.data());

        uint32_t market_id = abi::decode_uint32(args.data() + 32 + 28);

        auto pos = dex_.vault().get_position(account, market_id);
        if (!pos) return {};

        // Encode LXPosition
        std::vector<uint8_t> result(224);
        abi::encode_int128(result.data(), static_cast<I128>(pos->market_id));
        result[63] = static_cast<uint8_t>(pos->side);
        abi::encode_int128(result.data() + 64, pos->size_x18);
        abi::encode_int128(result.data() + 96, pos->entry_px_x18);
        abi::encode_int128(result.data() + 128, pos->unrealized_pnl_x18);
        abi::encode_int128(result.data() + 160, pos->accumulated_funding_x18);
        abi::encode_uint64(result.data() + 192 + 24, pos->last_funding_time);
        return result;
    };

    // getBalance(address,address) -> 0xf8b2cb4f
    handlers_[vault_key][0xf8b2cb4f] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 64) return {};

        LXAccount account;
        account.main = abi::decode_address(args.data());

        Currency token;
        token.addr = abi::decode_address(args.data() + 32);

        I128 balance = dex_.vault().get_balance(account, token);
        return abi::encode_int128_vec(balance);
    };

    // getMarginInfo(address) -> 0x6d435421
    handlers_[vault_key][0x6d435421] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        LXAccount account;
        account.main = abi::decode_address(args.data());

        LXMarginInfo info = dex_.vault().get_margin_info(account);

        // Encode LXMarginInfo
        std::vector<uint8_t> result(192);
        abi::encode_int128(result.data(), info.total_collateral_x18);
        abi::encode_int128(result.data() + 32, info.used_margin_x18);
        abi::encode_int128(result.data() + 64, info.free_margin_x18);
        abi::encode_int128(result.data() + 96, info.margin_ratio_x18);
        abi::encode_int128(result.data() + 128, info.maintenance_margin_x18);
        result[160 + 31] = info.liquidatable ? 1 : 0;
        return result;
    };

    // isLiquidatable(address) -> 0x8a7c195f
    handlers_[vault_key][0x8a7c195f] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        LXAccount account;
        account.main = abi::decode_address(args.data());

        bool liquidatable = dex_.vault().is_liquidatable(account);
        return abi::encode_bool(liquidatable);
    };

    // liquidate(address,address,uint32,int128) -> 0x2e1a7d4d
    handlers_[vault_key][0x2e1a7d4d] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 128) return {};

        LXAccount liquidator;
        liquidator.main = abi::decode_address(args.data());

        LXAccount account;
        account.main = abi::decode_address(args.data() + 32);

        uint32_t market_id = abi::decode_uint32(args.data() + 64 + 28);
        I128 size_x18 = abi::decode_int128(args.data() + 96);

        LXLiquidationResult result = dex_.vault().liquidate(liquidator, account, market_id, size_x18);

        // Encode result
        std::vector<uint8_t> encoded(192);
        abi::encode_int128(encoded.data(), static_cast<I128>(result.market_id));
        abi::encode_int128(encoded.data() + 32, result.size_x18);
        abi::encode_int128(encoded.data() + 64, result.price_x18);
        abi::encode_int128(encoded.data() + 96, result.penalty_x18);
        encoded[128 + 31] = result.adl_triggered ? 1 : 0;
        return encoded;
    };
}

// =============================================================================
// Handler Registration - LXOracle (LP-9011)
// =============================================================================

void PrecompileRouter::register_oracle_handlers() {
    uint64_t oracle_key = addresses::to_lp(addresses::LX_ORACLE);

    // getPrice(uint64) -> 0x99cff17c
    handlers_[oracle_key][0x99cff17c] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint64_t asset_id = abi::decode_uint64(args.data() + 24);

        auto price = dex_.oracle().get_price(asset_id);
        if (!price) return {};

        return abi::encode_int128_vec(*price);
    };

    // getPriceData(uint64) -> 0x3d18b912
    handlers_[oracle_key][0x3d18b912] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint64_t asset_id = abi::decode_uint64(args.data() + 24);

        auto data = dex_.oracle().get_price_data(asset_id);
        if (!data) return {};

        // Encode AggregatedPriceData
        std::vector<uint8_t> result(192);
        abi::encode_int128(result.data(), data->price_x18);
        abi::encode_int128(result.data() + 32, data->confidence_x18);
        abi::encode_int128(result.data() + 64, data->deviation_x18);
        result[96 + 31] = data->num_sources;
        abi::encode_uint64(result.data() + 128 + 24, data->timestamp);
        result[160 + 31] = static_cast<uint8_t>(data->method);
        return result;
    };

    // updatePrice(uint64,uint8,int128,int128) -> 0x7d3e47c1
    handlers_[oracle_key][0x7d3e47c1] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 128) return {};

        uint64_t asset_id = abi::decode_uint64(args.data() + 24);
        PriceSource source = static_cast<PriceSource>(args[32 + 31]);
        I128 price_x18 = abi::decode_int128(args.data() + 64);
        I128 confidence_x18 = abi::decode_int128(args.data() + 96);

        int32_t result = dex_.oracle().update_price(asset_id, source, price_x18, confidence_x18);
        return abi::encode_int32(result);
    };

    // indexPrice(uint64) -> 0xa1b2c3d4
    handlers_[oracle_key][0xa1b2c3d4] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint64_t asset_id = abi::decode_uint64(args.data() + 24);

        auto price = dex_.oracle().index_price(asset_id);
        if (!price) return {};

        return abi::encode_int128_vec(*price);
    };

    // getTwap(uint64,uint64) -> 0xb2c3d4e5
    handlers_[oracle_key][0xb2c3d4e5] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 64) return {};

        uint64_t asset_id = abi::decode_uint64(args.data() + 24);
        uint64_t window = abi::decode_uint64(args.data() + 32 + 24);

        auto twap = dex_.oracle().get_twap(asset_id, window);
        if (!twap) return {};

        return abi::encode_int128_vec(*twap);
    };

    // isPriceFresh(uint64) -> 0xc3d4e5f6
    handlers_[oracle_key][0xc3d4e5f6] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint64_t asset_id = abi::decode_uint64(args.data() + 24);

        bool fresh = dex_.oracle().is_price_fresh(asset_id);
        return abi::encode_bool(fresh);
    };
}

// =============================================================================
// Handler Registration - LXFeed (LP-9040)
// =============================================================================

void PrecompileRouter::register_feed_handlers() {
    uint64_t feed_key = addresses::to_lp(addresses::LX_FEED);

    // getMarkPrice(uint32) -> 0x82a0548d
    handlers_[feed_key][0x82a0548d] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint32_t market_id = abi::decode_uint32(args.data() + 28);

        auto mark = dex_.feed().get_mark_price(market_id);
        if (!mark) return {};

        // Encode LXMarkPrice: index, mark, premium, timestamp
        std::vector<uint8_t> result(128);
        abi::encode_int128(result.data(), mark->index_px_x18);
        abi::encode_int128(result.data() + 32, mark->mark_px_x18);
        abi::encode_int128(result.data() + 64, mark->premium_x18);
        abi::encode_uint64(result.data() + 96 + 24, mark->timestamp);
        return result;
    };

    // getFundingRate(uint32) -> 0x8c6f037f
    handlers_[feed_key][0x8c6f037f] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint32_t market_id = abi::decode_uint32(args.data() + 28);

        auto rate = dex_.feed().get_funding_rate(market_id);
        if (!rate) return {};

        // Encode LXFundingRate: rate, next_funding_time
        std::vector<uint8_t> result(64);
        abi::encode_int128(result.data(), rate->rate_x18);
        abi::encode_uint64(result.data() + 32 + 24, rate->next_funding_time);
        return result;
    };

    // indexPrice(uint32) -> 0x9d0e1f2a
    handlers_[feed_key][0x9d0e1f2a] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint32_t market_id = abi::decode_uint32(args.data() + 28);

        auto price = dex_.feed().index_price(market_id);
        if (!price) return {};

        return abi::encode_int128_vec(*price);
    };

    // markPrice(uint32) -> 0xae1f2b3c
    handlers_[feed_key][0xae1f2b3c] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint32_t market_id = abi::decode_uint32(args.data() + 28);

        auto price = dex_.feed().mark_price(market_id);
        if (!price) return {};

        return abi::encode_int128_vec(*price);
    };

    // lastPrice(uint32) -> 0xbf2a3c4d
    handlers_[feed_key][0xbf2a3c4d] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint32_t market_id = abi::decode_uint32(args.data() + 28);

        auto price = dex_.feed().last_price(market_id);
        if (!price) return {};

        return abi::encode_int128_vec(*price);
    };

    // midPrice(uint32) -> 0xc03b4d5e
    handlers_[feed_key][0xc03b4d5e] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint32_t market_id = abi::decode_uint32(args.data() + 28);

        auto price = dex_.feed().mid_price(market_id);
        if (!price) return {};

        return abi::encode_int128_vec(*price);
    };

    // getAllPrices(uint32) -> 0xd14c5e6f
    handlers_[feed_key][0xd14c5e6f] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint32_t market_id = abi::decode_uint32(args.data() + 28);

        auto prices = dex_.feed().get_all_prices(market_id);
        if (!prices) return {};

        // Encode AllPrices: index, mark, last, mid, timestamp
        std::vector<uint8_t> result(160);
        abi::encode_int128(result.data(), prices->index_x18);
        abi::encode_int128(result.data() + 32, prices->mark_x18);
        abi::encode_int128(result.data() + 64, prices->last_x18);
        abi::encode_int128(result.data() + 96, prices->mid_x18);
        abi::encode_uint64(result.data() + 128 + 24, prices->timestamp);
        return result;
    };

    // premium(uint32) -> 0xe25d6f70
    handlers_[feed_key][0xe25d6f70] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint32_t market_id = abi::decode_uint32(args.data() + 28);

        auto premium = dex_.feed().premium(market_id);
        if (!premium) return {};

        return abi::encode_int128_vec(*premium);
    };

    // basis(uint32) -> 0xf36e7081
    handlers_[feed_key][0xf36e7081] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint32_t market_id = abi::decode_uint32(args.data() + 28);

        auto basis = dex_.feed().basis(market_id);
        if (!basis) return {};

        return abi::encode_int128_vec(*basis);
    };

    // fundingInterval(uint32) -> 0x047f8192
    handlers_[feed_key][0x047f8192] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint32_t market_id = abi::decode_uint32(args.data() + 28);

        uint64_t interval = dex_.feed().funding_interval(market_id);

        std::vector<uint8_t> result(32, 0);
        abi::encode_uint64(result.data() + 24, interval);
        return result;
    };

    // predictedFundingRate(uint32) -> 0x158092a3
    handlers_[feed_key][0x158092a3] = [this](const std::vector<uint8_t>& args) -> std::vector<uint8_t> {
        if (args.size() < 32) return {};

        uint32_t market_id = abi::decode_uint32(args.data() + 28);

        auto rate = dex_.feed().predicted_funding_rate(market_id);
        if (!rate) return {};

        return abi::encode_int128_vec(*rate);
    };
}

} // namespace lux
