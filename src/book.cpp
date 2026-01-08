// =============================================================================
// book.cpp - LXBook CLOB Matching Engine Wrapper (LP-9020)
// =============================================================================

#include "lux/book.hpp"
#include <chrono>
#include <algorithm>
#include <cstring>

namespace lux {

// =============================================================================
// BookTradeListener Implementation
// =============================================================================

void LXBook::BookTradeListener::on_trade(const Trade& trade) {
    book_->record_trade(static_cast<uint32_t>(trade.symbol_id), trade);

    // Call settlement callback if set
    if (book_->settlement_callback_) {
        std::vector<Trade> trades{trade};
        book_->settlement_callback_(trades);
    }
}

void LXBook::BookTradeListener::on_order_filled(const Order& order) {
    book_->update_order_state(
        LXAccount{{}, 0}, // Would need proper account tracking
        order.id,
        [](BookOrderState& state) {
            state.status = BookOrderStatus::FILLED;
            state.remaining_size_x18 = 0;
        }
    );
}

void LXBook::BookTradeListener::on_order_partially_filled(const Order& order, Quantity fill_qty) {
    book_->update_order_state(
        LXAccount{{}, 0},
        order.id,
        [fill_qty](BookOrderState& state) {
            // Convert fill_qty to X18
            I128 fill_x18 = static_cast<I128>(fill_qty) * X18_ONE / 100000000LL; // From 1e8 to 1e18
            state.filled_size_x18 += fill_x18;
            state.remaining_size_x18 -= fill_x18;
        }
    );
}

void LXBook::BookTradeListener::on_order_cancelled(const Order& order) {
    book_->update_order_state(
        LXAccount{{}, 0},
        order.id,
        [](BookOrderState& state) {
            state.status = BookOrderStatus::CANCELLED;
        }
    );
}

// =============================================================================
// Constructor
// =============================================================================

LXBook::LXBook() : engine_(EngineConfig{}) {
    trade_listener_ = std::make_unique<BookTradeListener>(this);
    engine_.set_trade_listener(trade_listener_.get());
}

// =============================================================================
// Market Management
// =============================================================================

int32_t LXBook::create_market(const BookMarketConfig& config) {
    std::unique_lock lock(markets_mutex_);

    if (markets_.find(config.market_id) != markets_.end()) {
        return errors::POOL_ALREADY_INITIALIZED;
    }

    // Add symbol to engine
    if (!engine_.add_symbol(config.symbol_id)) {
        return errors::POOL_ALREADY_INITIALIZED;
    }

    markets_[config.market_id] = config;
    market_to_symbol_[config.market_id] = config.symbol_id;

    return errors::OK;
}

int32_t LXBook::update_market_config(const BookMarketConfig& config) {
    std::unique_lock lock(markets_mutex_);

    auto it = markets_.find(config.market_id);
    if (it == markets_.end()) {
        return errors::MARKET_NOT_FOUND;
    }

    it->second = config;
    return errors::OK;
}

std::optional<BookMarketConfig> LXBook::get_market_config(uint32_t market_id) const {
    std::shared_lock lock(markets_mutex_);
    auto it = markets_.find(market_id);
    if (it == markets_.end()) return std::nullopt;
    return it->second;
}

uint8_t LXBook::get_market_status(uint32_t market_id) const {
    std::shared_lock lock(markets_mutex_);
    auto it = markets_.find(market_id);
    if (it == markets_.end()) return 0;
    return it->second.status;
}

bool LXBook::market_exists(uint32_t market_id) const {
    std::shared_lock lock(markets_mutex_);
    return markets_.find(market_id) != markets_.end();
}

// =============================================================================
// Execute Interface
// =============================================================================

ExecuteResult LXBook::execute(const LXAccount& sender, const LXAction& action) {
    ExecuteResult result;
    result.error_code = errors::OK;

    switch (action.action_type) {
        case ActionType::PLACE:
            result = handle_place(sender, action.data);
            break;
        case ActionType::CANCEL:
            result = handle_cancel(sender, action.data);
            break;
        case ActionType::CANCEL_BY_CLOID:
            result = handle_cancel_by_cloid(sender, action.data);
            break;
        case ActionType::MODIFY:
            result = handle_modify(sender, action.data);
            break;
        case ActionType::NOOP:
            break;
        default:
            result.error_code = errors::UNAUTHORIZED;
    }

    return result;
}

std::vector<ExecuteResult> LXBook::execute_batch(const LXAccount& sender,
                                                  const std::vector<LXAction>& actions) {
    std::vector<ExecuteResult> results;
    results.reserve(actions.size());

    for (const auto& action : actions) {
        results.push_back(execute(sender, action));
    }

    return results;
}

// =============================================================================
// Order Operations
// =============================================================================

LXPlaceResult LXBook::place_order(const LXAccount& sender, const LXOrder& order) {
    LXPlaceResult result{};

    uint64_t symbol_id = get_symbol_id(order.market_id);
    if (symbol_id == 0) {
        result.status = static_cast<uint8_t>(BookOrderStatus::REJECTED);
        return result;
    }

    // Check market status
    std::shared_lock lock(markets_mutex_);
    auto market_it = markets_.find(order.market_id);
    if (market_it == markets_.end()) {
        result.status = static_cast<uint8_t>(BookOrderStatus::REJECTED);
        return result;
    }

    const BookMarketConfig& config = market_it->second;
    if (config.status == 0) { // Inactive
        result.status = static_cast<uint8_t>(BookOrderStatus::REJECTED);
        return result;
    }
    if (config.status == 2 && order.kind != OrderKind::LIMIT) { // Cancel-only
        result.status = static_cast<uint8_t>(BookOrderStatus::REJECTED);
        return result;
    }
    lock.unlock();

    // Convert to internal order format
    Order internal_order = convert_to_internal(order, symbol_id, sender);

    // Place order on engine
    OrderResult engine_result = engine_.place_order(internal_order);

    result.oid = engine_result.order_id;
    result.status = engine_result.success ?
        static_cast<uint8_t>(BookOrderStatus::NEW) :
        static_cast<uint8_t>(BookOrderStatus::REJECTED);

    // Calculate fills
    I128 total_fill_size = 0;
    I128 total_fill_value = 0;
    for (const auto& trade : engine_result.trades) {
        I128 trade_size = static_cast<I128>(trade.quantity) * X18_ONE / 100000000LL;
        I128 trade_price = static_cast<I128>(trade.price) * X18_ONE / 100000000LL;
        total_fill_size += trade_size;
        total_fill_value += x18::mul(trade_size, trade_price);
    }

    result.filled_size_x18 = total_fill_size;
    if (total_fill_size > 0) {
        result.avg_px_x18 = x18::div(total_fill_value, total_fill_size);
    }

    // Track order state
    if (engine_result.success) {
        std::unique_lock orders_lock(orders_mutex_);
        auto& account_orders = account_orders_[sender.hash()];

        BookOrderState state;
        state.oid = result.oid;
        state.cloid = order.cloid;
        state.market_id = order.market_id;
        state.is_buy = order.is_buy;
        state.kind = order.kind;
        state.tif = order.tif;
        state.original_size_x18 = order.size_x18;
        state.remaining_size_x18 = order.size_x18 - result.filled_size_x18;
        state.filled_size_x18 = result.filled_size_x18;
        state.limit_price_x18 = order.limit_px_x18;
        state.trigger_price_x18 = order.trigger_px_x18;
        state.avg_fill_price_x18 = result.avg_px_x18;
        state.status = (state.remaining_size_x18 == 0) ?
            BookOrderStatus::FILLED : BookOrderStatus::OPEN;
        state.created_at = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
        state.updated_at = state.created_at;
        state.flags = order.reduce_only ? fill_flags::REDUCE_ONLY : 0;

        account_orders.orders[result.oid] = state;
        account_orders.cloid_to_oid[order.cloid] = result.oid;
    }

    total_orders_placed_.fetch_add(1, std::memory_order_relaxed);

    return result;
}

int32_t LXBook::cancel_order(const LXAccount& sender, uint32_t market_id, uint64_t oid) {
    uint64_t symbol_id = get_symbol_id(market_id);
    if (symbol_id == 0) {
        return errors::MARKET_NOT_FOUND;
    }

    CancelResult result = engine_.cancel_order(symbol_id, oid);
    if (!result.success) {
        return errors::ORDER_NOT_FOUND;
    }

    // Update order state
    update_order_state(sender, oid, [](BookOrderState& state) {
        state.status = BookOrderStatus::CANCELLED;
    });

    return errors::OK;
}

int32_t LXBook::cancel_by_cloid(const LXAccount& sender, uint32_t market_id,
                                 const std::array<uint8_t, 16>& cloid) {
    std::shared_lock lock(orders_mutex_);
    auto account_it = account_orders_.find(sender.hash());
    if (account_it == account_orders_.end()) {
        return errors::ORDER_NOT_FOUND;
    }

    auto cloid_it = account_it->second.cloid_to_oid.find(cloid);
    if (cloid_it == account_it->second.cloid_to_oid.end()) {
        return errors::ORDER_NOT_FOUND;
    }

    uint64_t oid = cloid_it->second;
    lock.unlock();

    return cancel_order(sender, market_id, oid);
}

int32_t LXBook::cancel_all(const LXAccount& sender, uint32_t market_id) {
    std::shared_lock lock(orders_mutex_);
    auto account_it = account_orders_.find(sender.hash());
    if (account_it == account_orders_.end()) {
        return errors::OK; // No orders to cancel
    }

    std::vector<uint64_t> oids_to_cancel;
    for (const auto& [oid, state] : account_it->second.orders) {
        if (state.market_id == market_id &&
            (state.status == BookOrderStatus::OPEN || state.status == BookOrderStatus::NEW)) {
            oids_to_cancel.push_back(oid);
        }
    }
    lock.unlock();

    for (uint64_t oid : oids_to_cancel) {
        cancel_order(sender, market_id, oid);
    }

    return errors::OK;
}

LXPlaceResult LXBook::amend_order(const LXAccount& sender, uint32_t market_id,
                                   uint64_t oid, I128 new_size_x18, I128 new_price_x18) {
    LXPlaceResult result{};

    uint64_t symbol_id = get_symbol_id(market_id);
    if (symbol_id == 0) {
        result.status = static_cast<uint8_t>(BookOrderStatus::REJECTED);
        return result;
    }

    // Convert to internal units
    Price new_price = static_cast<Price>(x18::to_double(new_price_x18) * 100000000.0);
    Quantity new_qty = static_cast<Quantity>(x18::to_double(new_size_x18) * 100000000.0);

    OrderResult modify_result = engine_.modify_order(symbol_id, oid, new_price, new_qty);

    if (!modify_result.success) {
        result.status = static_cast<uint8_t>(BookOrderStatus::REJECTED);
        return result;
    }

    result.oid = oid;
    result.status = static_cast<uint8_t>(BookOrderStatus::OPEN);

    // Update order state
    update_order_state(sender, oid, [new_size_x18, new_price_x18](BookOrderState& state) {
        state.remaining_size_x18 = new_size_x18;
        state.limit_price_x18 = new_price_x18;
        state.updated_at = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
    });

    return result;
}

// =============================================================================
// Order Queries
// =============================================================================

std::optional<BookOrderState> LXBook::get_order(uint32_t market_id, uint64_t oid) const {
    std::shared_lock lock(orders_mutex_);

    for (const auto& [account_hash, account_orders] : account_orders_) {
        auto it = account_orders.orders.find(oid);
        if (it != account_orders.orders.end() && it->second.market_id == market_id) {
            return it->second;
        }
    }

    return std::nullopt;
}

std::optional<BookOrderState> LXBook::get_order_by_cloid(uint32_t market_id,
                                                          const std::array<uint8_t, 16>& cloid) const {
    std::shared_lock lock(orders_mutex_);

    for (const auto& [account_hash, account_orders] : account_orders_) {
        auto cloid_it = account_orders.cloid_to_oid.find(cloid);
        if (cloid_it != account_orders.cloid_to_oid.end()) {
            auto order_it = account_orders.orders.find(cloid_it->second);
            if (order_it != account_orders.orders.end() &&
                order_it->second.market_id == market_id) {
                return order_it->second;
            }
        }
    }

    return std::nullopt;
}

std::vector<BookOrderState> LXBook::get_orders(const LXAccount& account, uint32_t market_id) const {
    std::vector<BookOrderState> orders;

    std::shared_lock lock(orders_mutex_);
    auto account_it = account_orders_.find(account.hash());
    if (account_it == account_orders_.end()) {
        return orders;
    }

    for (const auto& [oid, state] : account_it->second.orders) {
        if (state.market_id == market_id) {
            orders.push_back(state);
        }
    }

    return orders;
}

std::vector<BookOrderState> LXBook::get_all_orders(const LXAccount& account) const {
    std::vector<BookOrderState> orders;

    std::shared_lock lock(orders_mutex_);
    auto account_it = account_orders_.find(account.hash());
    if (account_it == account_orders_.end()) {
        return orders;
    }

    for (const auto& [oid, state] : account_it->second.orders) {
        orders.push_back(state);
    }

    return orders;
}

// =============================================================================
// Market Data
// =============================================================================

LXL1 LXBook::get_l1(uint32_t market_id) const {
    LXL1 l1{};

    uint64_t symbol_id = get_symbol_id(market_id);
    if (symbol_id == 0) return l1;

    auto best_bid = engine_.best_bid(symbol_id);
    auto best_ask = engine_.best_ask(symbol_id);

    if (best_bid) {
        l1.best_bid_px_x18 = static_cast<I128>(*best_bid) * X18_ONE / 100000000LL;
    }
    if (best_ask) {
        l1.best_ask_px_x18 = static_cast<I128>(*best_ask) * X18_ONE / 100000000LL;
    }

    // Get last trade
    std::shared_lock lock(trades_mutex_);
    auto last_it = last_trades_.find(market_id);
    if (last_it != last_trades_.end()) {
        l1.last_trade_px_x18 = static_cast<I128>(last_it->second.price) * X18_ONE / 100000000LL;
    }

    return l1;
}

MarketDepth LXBook::get_depth(uint32_t market_id, size_t levels) const {
    uint64_t symbol_id = get_symbol_id(market_id);
    if (symbol_id == 0) return MarketDepth{};

    return engine_.get_depth(symbol_id, levels);
}

std::optional<Trade> LXBook::get_last_trade(uint32_t market_id) const {
    std::shared_lock lock(trades_mutex_);
    auto it = last_trades_.find(market_id);
    if (it == last_trades_.end()) return std::nullopt;
    return it->second;
}

std::vector<Trade> LXBook::get_recent_trades(uint32_t market_id, size_t count) const {
    std::shared_lock lock(trades_mutex_);
    auto it = recent_trades_.find(market_id);
    if (it == recent_trades_.end()) return {};

    const auto& trades = it->second;
    if (trades.size() <= count) {
        return trades;
    }

    return std::vector<Trade>(trades.end() - count, trades.end());
}

// =============================================================================
// HFT Interface
// =============================================================================

std::vector<uint8_t> LXBook::execute_packed(const std::vector<uint8_t>& packed_data) {
    if (packed_data.size() < sizeof(packed::PackedPlaceOrder)) {
        return {};
    }

    // Determine action type from first byte pattern
    // For now, assume PlaceOrder
    packed::PackedPlaceOrder packed;
    std::memcpy(&packed, packed_data.data(), sizeof(packed));

    LXOrder order;
    order.market_id = packed.market_id;
    order.is_buy = packed.flags & packed::FLAG_IS_BUY;
    order.kind = static_cast<OrderKind>((packed.flags & packed::FLAG_KIND_MASK) >> packed::FLAG_KIND_SHIFT);
    order.tif = static_cast<TIF>((packed.flags & packed::FLAG_TIF_MASK) >> packed::FLAG_TIF_SHIFT);
    order.reduce_only = packed.flags & packed::FLAG_REDUCE_ONLY;
    order.size_x18 = static_cast<I128>(packed.size) * X18_ONE / 100000000LL;
    order.limit_px_x18 = static_cast<I128>(packed.limit_price) * X18_ONE / 100000000LL;
    order.trigger_px_x18 = static_cast<I128>(packed.trigger_price) * X18_ONE / 100000000LL;

    LXAccount sender{}; // Would come from authenticated context
    LXPlaceResult result = place_order(sender, order);

    // Pack result
    packed::PackedPlaceResult packed_result;
    packed_result.oid = result.oid;
    packed_result.status = result.status;
    packed_result.filled_size = static_cast<int64_t>(result.filled_size_x18 * 100000000LL / X18_ONE);
    packed_result.avg_price = static_cast<int64_t>(result.avg_px_x18 * 100000000LL / X18_ONE);

    std::vector<uint8_t> response(sizeof(packed_result));
    std::memcpy(response.data(), &packed_result, sizeof(packed_result));

    return response;
}

std::vector<uint8_t> LXBook::execute_batch_packed(const std::vector<uint8_t>& packed_data) {
    // Parse batch header and execute each order
    // Simplified: just forward to single execute
    return execute_packed(packed_data);
}

// =============================================================================
// Settlement Integration
// =============================================================================

void LXBook::set_settlement_callback(SettlementCallback callback) {
    settlement_callback_ = std::move(callback);
}

// =============================================================================
// Statistics
// =============================================================================

LXBook::Stats LXBook::get_stats() const {
    Engine::Stats engine_stats = engine_.get_stats();

    std::shared_lock lock(markets_mutex_);

    return Stats{
        markets_.size(),
        total_orders_placed_.load(std::memory_order_relaxed),
        engine_stats.total_orders_cancelled,
        total_orders_filled_.load(std::memory_order_relaxed),
        engine_stats.total_trades,
        static_cast<I128>(engine_stats.total_volume) * X18_ONE / 100000000LL
    };
}

// =============================================================================
// Internal Helpers
// =============================================================================

uint64_t LXBook::get_symbol_id(uint32_t market_id) const {
    std::shared_lock lock(markets_mutex_);
    auto it = market_to_symbol_.find(market_id);
    return (it != market_to_symbol_.end()) ? it->second : 0;
}

Order LXBook::convert_to_internal(const LXOrder& order, uint64_t symbol_id,
                                   const LXAccount& sender) const {
    Order internal;
    internal.id = OrderIdGenerator::instance().next();
    internal.symbol_id = symbol_id;
    internal.account_id = sender.hash();
    internal.side = order.is_buy ? Side::Buy : Side::Sell;

    // Convert X18 to internal units (1e8)
    internal.price = static_cast<Price>(x18::to_double(order.limit_px_x18) * 100000000.0);
    internal.quantity = static_cast<Quantity>(x18::to_double(order.size_x18) * 100000000.0);
    internal.filled = 0;

    // Map order types
    switch (order.kind) {
        case OrderKind::LIMIT:
            internal.type = OrderType::Limit;
            break;
        case OrderKind::MARKET:
            internal.type = OrderType::Market;
            break;
        case OrderKind::STOP_MARKET:
        case OrderKind::STOP_LIMIT:
            internal.type = OrderType::StopLimit;
            internal.stop_price = static_cast<Price>(
                x18::to_double(order.trigger_px_x18) * 100000000.0
            );
            break;
        default:
            internal.type = OrderType::Limit;
    }

    // Map TIF
    switch (order.tif) {
        case TIF::GTC:
            internal.tif = TimeInForce::GTC;
            break;
        case TIF::IOC:
            internal.tif = TimeInForce::IOC;
            break;
        case TIF::ALO:
            internal.tif = TimeInForce::GTC; // ALO is GTC with post-only flag
            break;
    }

    internal.status = OrderStatus::New;
    internal.stp_group = 0;
    internal.timestamp = std::chrono::duration_cast<Timestamp>(
        std::chrono::system_clock::now().time_since_epoch()
    );

    return internal;
}

void LXBook::update_order_state(const LXAccount& account, uint64_t oid,
                                 const std::function<void(BookOrderState&)>& updater) {
    std::unique_lock lock(orders_mutex_);

    // Try to find by account first
    auto account_it = account_orders_.find(account.hash());
    if (account_it != account_orders_.end()) {
        auto order_it = account_it->second.orders.find(oid);
        if (order_it != account_it->second.orders.end()) {
            updater(order_it->second);
            return;
        }
    }

    // Search all accounts if not found
    for (auto& [acc_hash, acc_orders] : account_orders_) {
        auto order_it = acc_orders.orders.find(oid);
        if (order_it != acc_orders.orders.end()) {
            updater(order_it->second);
            return;
        }
    }
}

void LXBook::record_trade(uint32_t market_id, const Trade& trade) {
    std::unique_lock lock(trades_mutex_);

    last_trades_[market_id] = trade;

    auto& trades = recent_trades_[market_id];
    trades.push_back(trade);

    // Keep only last 1000 trades
    if (trades.size() > 1000) {
        trades.erase(trades.begin(), trades.begin() + (trades.size() - 1000));
    }
}

// =============================================================================
// Action Handlers
// =============================================================================

ExecuteResult LXBook::handle_place(const LXAccount& sender, const std::vector<uint8_t>& data) {
    ExecuteResult result;
    result.error_code = errors::OK;

    // Parse order from data
    // Simplified: expect packed format
    if (data.size() < sizeof(packed::PackedPlaceOrder)) {
        result.error_code = errors::INVALID_PRICE;
        return result;
    }

    packed::PackedPlaceOrder packed;
    std::memcpy(&packed, data.data(), sizeof(packed));

    LXOrder order;
    order.market_id = packed.market_id;
    order.is_buy = packed.flags & packed::FLAG_IS_BUY;
    order.kind = static_cast<OrderKind>((packed.flags & packed::FLAG_KIND_MASK) >> packed::FLAG_KIND_SHIFT);
    order.tif = static_cast<TIF>((packed.flags & packed::FLAG_TIF_MASK) >> packed::FLAG_TIF_SHIFT);
    order.reduce_only = packed.flags & packed::FLAG_REDUCE_ONLY;
    order.size_x18 = static_cast<I128>(packed.size) * X18_ONE / 100000000LL;
    order.limit_px_x18 = static_cast<I128>(packed.limit_price) * X18_ONE / 100000000LL;
    order.trigger_px_x18 = static_cast<I128>(packed.trigger_price) * X18_ONE / 100000000LL;

    LXPlaceResult place_result = place_order(sender, order);

    // Pack result
    packed::PackedPlaceResult packed_result;
    packed_result.oid = place_result.oid;
    packed_result.status = place_result.status;
    packed_result.filled_size = static_cast<int64_t>(place_result.filled_size_x18 * 100000000LL / X18_ONE);
    packed_result.avg_price = static_cast<int64_t>(place_result.avg_px_x18 * 100000000LL / X18_ONE);

    result.result_data.resize(sizeof(packed_result));
    std::memcpy(result.result_data.data(), &packed_result, sizeof(packed_result));

    return result;
}

ExecuteResult LXBook::handle_cancel(const LXAccount& sender, const std::vector<uint8_t>& data) {
    ExecuteResult result;

    if (data.size() < sizeof(packed::PackedCancelOrder)) {
        result.error_code = errors::INVALID_PRICE;
        return result;
    }

    packed::PackedCancelOrder packed;
    std::memcpy(&packed, data.data(), sizeof(packed));

    result.error_code = cancel_order(sender, packed.market_id, packed.oid);

    return result;
}

ExecuteResult LXBook::handle_cancel_by_cloid(const LXAccount& sender, const std::vector<uint8_t>& data) {
    ExecuteResult result;

    if (data.size() < sizeof(uint32_t) + 16) {
        result.error_code = errors::INVALID_PRICE;
        return result;
    }

    uint32_t market_id;
    std::array<uint8_t, 16> cloid;

    std::memcpy(&market_id, data.data(), sizeof(market_id));
    std::memcpy(cloid.data(), data.data() + sizeof(market_id), 16);

    result.error_code = cancel_by_cloid(sender, market_id, cloid);

    return result;
}

ExecuteResult LXBook::handle_modify(const LXAccount& sender, const std::vector<uint8_t>& data) {
    ExecuteResult result;
    result.error_code = errors::OK;

    // Parse modify request
    if (data.size() < sizeof(uint32_t) + sizeof(uint64_t) + 2 * sizeof(int64_t)) {
        result.error_code = errors::INVALID_PRICE;
        return result;
    }

    size_t offset = 0;
    uint32_t market_id;
    uint64_t oid;
    int64_t new_size;
    int64_t new_price;

    std::memcpy(&market_id, data.data() + offset, sizeof(market_id));
    offset += sizeof(market_id);
    std::memcpy(&oid, data.data() + offset, sizeof(oid));
    offset += sizeof(oid);
    std::memcpy(&new_size, data.data() + offset, sizeof(new_size));
    offset += sizeof(new_size);
    std::memcpy(&new_price, data.data() + offset, sizeof(new_price));

    I128 size_x18 = static_cast<I128>(new_size) * X18_ONE / 100000000LL;
    I128 price_x18 = static_cast<I128>(new_price) * X18_ONE / 100000000LL;

    LXPlaceResult amend_result = amend_order(sender, market_id, oid, size_x18, price_x18);

    // Pack result
    packed::PackedPlaceResult packed_result;
    packed_result.oid = amend_result.oid;
    packed_result.status = amend_result.status;
    packed_result.filled_size = 0;
    packed_result.avg_price = 0;

    result.result_data.resize(sizeof(packed_result));
    std::memcpy(result.result_data.data(), &packed_result, sizeof(packed_result));

    return result;
}

} // namespace lux
