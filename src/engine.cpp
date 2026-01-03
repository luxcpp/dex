#include "lux/engine.hpp"
#include <stdexcept>
#include <algorithm>

namespace lux {

Engine::Engine(EngineConfig config)
    : config_(std::move(config)) {}

Engine::~Engine() {
    stop();
}

void Engine::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;  // Already running
    }

    if (config_.async_mode && config_.worker_threads > 0) {
        for (size_t i = 0; i < config_.worker_threads; ++i) {
            worker_threads_.emplace_back(&Engine::worker_loop, this);
        }
    }
}

void Engine::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;  // Already stopped
    }

    // Wake up all workers
    queue_cv_.notify_all();

    // Join worker threads
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();
}

void Engine::worker_loop() {
    while (running_.load()) {
        AsyncOrder async_order;
        bool has_order = false;

        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !order_queue_.empty() || !running_.load();
            });

            if (!running_.load() && order_queue_.empty()) {
                return;
            }

            if (!order_queue_.empty()) {
                async_order = std::move(order_queue_.front());
                order_queue_.pop();
                has_order = true;
            }
        }

        if (has_order) {
            OrderResult result = process_single_order(async_order.batch_order);
            async_order.promise.set_value(std::move(result));
        }
    }
}

OrderResult Engine::process_single_order(const BatchOrder& batch_order) {
    switch (batch_order.action) {
        case BatchOrder::Action::Place:
            return place_order(batch_order.order);

        case BatchOrder::Action::Cancel: {
            auto cancel_result = cancel_order(
                batch_order.order.symbol_id,
                batch_order.order_id
            );
            OrderResult result;
            result.success = cancel_result.success;
            result.order_id = batch_order.order_id;
            result.error = cancel_result.error;
            return result;
        }

        case BatchOrder::Action::Modify:
            return modify_order(
                batch_order.order.symbol_id,
                batch_order.order_id,
                batch_order.new_price,
                batch_order.new_quantity
            );

        default:
            return {false, 0, "Unknown action", {}};
    }
}

bool Engine::add_symbol(uint64_t symbol_id) {
    std::unique_lock lock(orderbooks_mutex_);

    if (orderbooks_.find(symbol_id) != orderbooks_.end()) {
        return false;  // Symbol already exists
    }

    orderbooks_[symbol_id] = std::make_unique<OrderBook>(symbol_id);
    return true;
}

bool Engine::remove_symbol(uint64_t symbol_id) {
    std::unique_lock lock(orderbooks_mutex_);

    auto it = orderbooks_.find(symbol_id);
    if (it == orderbooks_.end()) {
        return false;
    }

    // Only remove if orderbook is empty
    if (it->second->total_orders() > 0) {
        return false;
    }

    orderbooks_.erase(it);
    return true;
}

bool Engine::has_symbol(uint64_t symbol_id) const {
    std::shared_lock lock(orderbooks_mutex_);
    return orderbooks_.find(symbol_id) != orderbooks_.end();
}

std::vector<uint64_t> Engine::symbols() const {
    std::shared_lock lock(orderbooks_mutex_);
    std::vector<uint64_t> result;
    result.reserve(orderbooks_.size());
    for (const auto& [id, _] : orderbooks_) {
        result.push_back(id);
    }
    return result;
}

OrderResult Engine::place_order(Order order) {
    OrderResult result;
    result.order_id = order.id;

    OrderBook* book = nullptr;
    {
        std::shared_lock lock(orderbooks_mutex_);
        auto it = orderbooks_.find(order.symbol_id);
        if (it == orderbooks_.end()) {
            result.success = false;
            result.error = "Unknown symbol";
            return result;
        }
        book = it->second.get();
    }

    try {
        result.trades = book->place_order(std::move(order), trade_listener_);
        result.success = true;

        // Update statistics
        total_orders_placed_.fetch_add(1, std::memory_order_relaxed);
        total_trades_.fetch_add(result.trades.size(), std::memory_order_relaxed);

        for (const auto& trade : result.trades) {
            total_volume_.fetch_add(trade.quantity, std::memory_order_relaxed);
        }

    } catch (const std::exception& e) {
        result.success = false;
        result.error = e.what();
    }

    return result;
}

CancelResult Engine::cancel_order(uint64_t symbol_id, uint64_t order_id) {
    CancelResult result;

    OrderBook* book = nullptr;
    {
        std::shared_lock lock(orderbooks_mutex_);
        auto it = orderbooks_.find(symbol_id);
        if (it == orderbooks_.end()) {
            result.success = false;
            result.error = "Unknown symbol";
            return result;
        }
        book = it->second.get();
    }

    result.cancelled_order = book->cancel_order(order_id);
    result.success = result.cancelled_order.has_value();

    if (!result.success) {
        result.error = "Order not found";
    } else {
        total_orders_cancelled_.fetch_add(1, std::memory_order_relaxed);

        if (trade_listener_) {
            trade_listener_->on_order_cancelled(*result.cancelled_order);
        }
    }

    return result;
}

OrderResult Engine::modify_order(uint64_t symbol_id, uint64_t order_id,
                                  Price new_price, Quantity new_quantity) {
    OrderResult result;
    result.order_id = order_id;

    OrderBook* book = nullptr;
    {
        std::shared_lock lock(orderbooks_mutex_);
        auto it = orderbooks_.find(symbol_id);
        if (it == orderbooks_.end()) {
            result.success = false;
            result.error = "Unknown symbol";
            return result;
        }
        book = it->second.get();
    }

    auto modified = book->modify_order(order_id, new_price, new_quantity);
    result.success = modified.has_value();

    if (!result.success) {
        result.error = "Order not found";
    }

    return result;
}

BatchResult Engine::process_batch(const std::vector<BatchOrder>& batch) {
    BatchResult result;

    // Group orders by symbol for locality
    std::unordered_map<uint64_t, std::vector<const BatchOrder*>> by_symbol;

    for (const auto& order : batch) {
        uint64_t symbol = order.action == BatchOrder::Action::Place ?
            order.order.symbol_id : order.order.symbol_id;
        by_symbol[symbol].push_back(&order);
    }

    // Process each symbol's orders
    for (const auto& [symbol_id, orders] : by_symbol) {
        OrderBook* book = nullptr;
        {
            std::shared_lock lock(orderbooks_mutex_);
            auto it = orderbooks_.find(symbol_id);
            if (it != orderbooks_.end()) {
                book = it->second.get();
            }
        }

        if (!book) {
            // All orders for unknown symbol fail
            for (const auto* batch_order : orders) {
                if (batch_order->action == BatchOrder::Action::Place) {
                    result.order_results.push_back({
                        false, batch_order->order.id, "Unknown symbol", {}
                    });
                } else {
                    result.cancel_results.push_back({
                        false, std::nullopt, "Unknown symbol"
                    });
                }
            }
            continue;
        }

        for (const auto* batch_order : orders) {
            switch (batch_order->action) {
                case BatchOrder::Action::Place: {
                    try {
                        auto trades = book->place_order(batch_order->order, trade_listener_);
                        result.order_results.push_back({
                            true, batch_order->order.id, "", std::move(trades)
                        });

                        for (const auto& trade : result.order_results.back().trades) {
                            result.all_trades.push_back(trade);
                        }

                        total_orders_placed_.fetch_add(1, std::memory_order_relaxed);

                    } catch (const std::exception& e) {
                        result.order_results.push_back({
                            false, batch_order->order.id, e.what(), {}
                        });
                    }
                    break;
                }

                case BatchOrder::Action::Cancel: {
                    auto cancelled = book->cancel_order(batch_order->order_id);
                    result.cancel_results.push_back({
                        cancelled.has_value(), cancelled, ""
                    });

                    if (cancelled) {
                        total_orders_cancelled_.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                }

                case BatchOrder::Action::Modify: {
                    auto modified = book->modify_order(
                        batch_order->order_id,
                        batch_order->new_price,
                        batch_order->new_quantity
                    );

                    result.order_results.push_back({
                        modified.has_value(),
                        batch_order->order_id,
                        modified ? "" : "Order not found",
                        {}
                    });
                    break;
                }
            }
        }
    }

    total_trades_.fetch_add(result.all_trades.size(), std::memory_order_relaxed);
    for (const auto& trade : result.all_trades) {
        total_volume_.fetch_add(trade.quantity, std::memory_order_relaxed);
    }

    return result;
}

std::optional<Order> Engine::get_order(uint64_t symbol_id, uint64_t order_id) const {
    std::shared_lock lock(orderbooks_mutex_);
    auto it = orderbooks_.find(symbol_id);
    if (it == orderbooks_.end()) {
        return std::nullopt;
    }
    return it->second->get_order(order_id);
}

MarketDepth Engine::get_depth(uint64_t symbol_id, size_t levels) const {
    std::shared_lock lock(orderbooks_mutex_);
    auto it = orderbooks_.find(symbol_id);
    if (it == orderbooks_.end()) {
        return {};
    }
    return it->second->get_depth(levels);
}

std::optional<Price> Engine::best_bid(uint64_t symbol_id) const {
    std::shared_lock lock(orderbooks_mutex_);
    auto it = orderbooks_.find(symbol_id);
    if (it == orderbooks_.end()) {
        return std::nullopt;
    }
    return it->second->best_bid();
}

std::optional<Price> Engine::best_ask(uint64_t symbol_id) const {
    std::shared_lock lock(orderbooks_mutex_);
    auto it = orderbooks_.find(symbol_id);
    if (it == orderbooks_.end()) {
        return std::nullopt;
    }
    return it->second->best_ask();
}

Engine::Stats Engine::get_stats() const {
    return {
        total_orders_placed_.load(std::memory_order_relaxed),
        total_orders_cancelled_.load(std::memory_order_relaxed),
        total_trades_.load(std::memory_order_relaxed),
        total_volume_.load(std::memory_order_relaxed)
    };
}

void Engine::set_trade_listener(TradeListener* listener) {
    trade_listener_ = listener;
}

OrderBook* Engine::get_orderbook(uint64_t symbol_id) {
    std::shared_lock lock(orderbooks_mutex_);
    auto it = orderbooks_.find(symbol_id);
    return it != orderbooks_.end() ? it->second.get() : nullptr;
}

const OrderBook* Engine::get_orderbook(uint64_t symbol_id) const {
    std::shared_lock lock(orderbooks_mutex_);
    auto it = orderbooks_.find(symbol_id);
    return it != orderbooks_.end() ? it->second.get() : nullptr;
}

} // namespace lux
