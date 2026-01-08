#ifndef LUX_LX_HPP
#define LUX_LX_HPP

// =============================================================================
// LX - Full DEX Stack
//
// LP-Aligned Precompile Addresses:
//   LP-9010: LXPool   (AMM Pool Manager)
//   LP-9011: LXOracle (Price Aggregation)
//   LP-9012: LXRouter (Swap Routing)
//   LP-9013: LXHooks  (Hook Registry)
//   LP-9014: LXFlash  (Flash Loans)
//   LP-9020: LXBook   (CLOB Matching)
//   LP-9030: LXVault  (Custody & Margin)
//   LP-9040: LXFeed   (Mark/Funding Prices)
//   LP-9050: LXLend   (Lending Pool)
//   LP-9060: LXLiquid (Self-Repaying Loans)
//
// =============================================================================

#include "types.hpp"
#include "order.hpp"
#include "trade.hpp"
#include "orderbook.hpp"
#include "engine.hpp"
#include "pool.hpp"
#include "book.hpp"
#include "vault.hpp"
#include "oracle.hpp"
#include "feed.hpp"

namespace lux {

// =============================================================================
// LX - Unified DEX Controller
// =============================================================================

class LX {
public:
    LX();
    ~LX();

    // Non-copyable
    LX(const LX&) = delete;
    LX& operator=(const LX&) = delete;

    // =========================================================================
    // Component Access
    // =========================================================================

    LXPool& pool() { return *pool_; }
    const LXPool& pool() const { return *pool_; }

    LXBook& book() { return *book_; }
    const LXBook& book() const { return *book_; }

    LXVault& vault() { return *vault_; }
    const LXVault& vault() const { return *vault_; }

    LXOracle& oracle() { return *oracle_; }
    const LXOracle& oracle() const { return *oracle_; }

    LXFeed& feed() { return *feed_; }
    const LXFeed& feed() const { return *feed_; }

    // =========================================================================
    // Initialization
    // =========================================================================

    // Initialize all components with default configurations
    void initialize();

    // Initialize with custom configurations
    struct Config {
        EngineConfig engine_config;
        bool enable_hooks;
        bool enable_flash_loans;
        uint64_t funding_interval;
        I128 default_maker_fee_x18;
        I128 default_taker_fee_x18;
    };
    void initialize(const Config& config);

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void start();
    void stop();
    bool is_running() const;

    // =========================================================================
    // Market Creation (Unified)
    // =========================================================================

    // Create a spot market (AMM pool)
    int32_t create_spot_market(const PoolKey& key, I128 sqrt_price_x96);

    // Create a perpetual market (CLOB + Vault)
    int32_t create_perp_market(uint32_t market_id, uint64_t asset_id,
                                const MarketConfig& vault_config,
                                const BookMarketConfig& book_config);

    // =========================================================================
    // Unified Trading Interface
    // =========================================================================

    // Smart order routing: AMM vs CLOB based on best execution
    BalanceDelta swap_smart(const LXAccount& sender, const Currency& token_in,
                            const Currency& token_out, I128 amount_in_x18,
                            I128 min_amount_out_x18);

    // Execute on best venue
    struct TradeResult {
        BalanceDelta delta;
        std::vector<Trade> trades;
        bool used_amm;
        bool used_clob;
        I128 effective_price_x18;
    };
    TradeResult trade(const LXAccount& sender, uint32_t market_id,
                      bool is_buy, I128 size_x18, I128 limit_price_x18);

    // =========================================================================
    // Cross-Component Operations
    // =========================================================================

    // Settle CLOB trades through vault
    int32_t settle_trades(const std::vector<Trade>& trades);

    // Update mark price from feed and accrue funding
    int32_t update_funding(uint32_t market_id);

    // Run liquidation check for all accounts
    int32_t run_liquidations(uint32_t market_id);

    // =========================================================================
    // Statistics
    // =========================================================================

    struct GlobalStats {
        LXPool::Stats pool_stats;
        LXBook::Stats book_stats;
        LXVault::Stats vault_stats;
        LXOracle::Stats oracle_stats;
        LXFeed::Stats feed_stats;
        uint64_t uptime_seconds;
    };
    GlobalStats get_stats() const;

    // =========================================================================
    // Version & Info
    // =========================================================================

    static constexpr const char* version() { return "1.0.0"; }

    struct ComponentInfo {
        const char* name;
        Address address;
        const char* description;
    };
    static std::vector<ComponentInfo> components() {
        return {
            {"LXPool",   addresses::LX_POOL,   "Uniswap v4-style AMM Pool Manager"},
            {"LXOracle", addresses::LX_ORACLE, "Multi-source Price Aggregation"},
            {"LXRouter", addresses::LX_ROUTER, "Smart Swap Routing"},
            {"LXHooks",  addresses::LX_HOOKS,  "Hook Contract Registry"},
            {"LXFlash",  addresses::LX_FLASH,  "Flash Loan Facility"},
            {"LXBook",   addresses::LX_BOOK,   "CLOB Matching Engine"},
            {"LXVault",  addresses::LX_VAULT,  "Custody & Margin System"},
            {"LXFeed",   addresses::LX_FEED,   "Mark/Index/Funding Prices"},
            {"LXLend",   addresses::LX_LEND,   "Lending Pool"},
            {"LXLiquid", addresses::LX_LIQUID, "Self-Repaying Loans"},
        };
    }

private:
    std::unique_ptr<LXPool> pool_;
    std::unique_ptr<LXOracle> oracle_;
    std::unique_ptr<LXVault> vault_;
    std::unique_ptr<LXBook> book_;
    std::unique_ptr<LXFeed> feed_;

    std::atomic<bool> running_{false};
    uint64_t start_time_{0};

    // Internal settlement callback
    int32_t on_book_trades(const std::vector<Trade>& trades);
};

// =============================================================================
// Precompile Router
// =============================================================================

class PrecompileRouter {
public:
    explicit PrecompileRouter(LX& dex);

    // Route call to appropriate precompile based on address
    std::vector<uint8_t> call(const Address& precompile,
                               const std::vector<uint8_t>& calldata);

    // Static call (read-only)
    std::vector<uint8_t> static_call(const Address& precompile,
                                      const std::vector<uint8_t>& calldata) const;

    // Check if address is a known precompile
    bool is_precompile(const Address& addr) const;

    // Get precompile gas cost
    uint64_t gas_cost(const Address& precompile,
                       const std::vector<uint8_t>& calldata) const;

private:
    LX& dex_;

    // Method selector -> handler mapping for each precompile
    using Handler = std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)>;
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, Handler>> handlers_;

    void register_pool_handlers();
    void register_book_handlers();
    void register_vault_handlers();
    void register_oracle_handlers();
    void register_feed_handlers();

    // ABI encoding/decoding helpers
    template<typename T>
    static T decode(const std::vector<uint8_t>& data, size_t offset = 0);

    template<typename... Args>
    static std::vector<uint8_t> encode(const Args&... args);
};

// =============================================================================
// Gas Costs (based on benchmarks)
// =============================================================================

namespace gas {

// LXPool (LP-9010)
constexpr uint64_t POOL_INITIALIZE = 50000;
constexpr uint64_t POOL_SWAP = 10000;
constexpr uint64_t POOL_MODIFY_LIQUIDITY = 20000;
constexpr uint64_t POOL_DONATE = 5000;
constexpr uint64_t POOL_FLASH = 5000;

// LXBook (LP-9020)
constexpr uint64_t BOOK_PLACE_ORDER = 15000;
constexpr uint64_t BOOK_CANCEL_ORDER = 5000;
constexpr uint64_t BOOK_EXECUTE = 20000;
constexpr uint64_t BOOK_EXECUTE_BATCH_BASE = 10000;
constexpr uint64_t BOOK_EXECUTE_BATCH_PER_ACTION = 5000;

// LXVault (LP-9030)
constexpr uint64_t VAULT_DEPOSIT = 10000;
constexpr uint64_t VAULT_WITHDRAW = 15000;
constexpr uint64_t VAULT_LIQUIDATE = 50000;
constexpr uint64_t VAULT_SETTLE = 10000;

// LXOracle (LP-9011)
constexpr uint64_t ORACLE_GET_PRICE = 2000;
constexpr uint64_t ORACLE_UPDATE_PRICE = 10000;

// LXFeed (LP-9040)
constexpr uint64_t FEED_GET_MARK_PRICE = 3000;
constexpr uint64_t FEED_GET_FUNDING_RATE = 2000;

} // namespace gas

} // namespace lux

#endif // LUX_LX_HPP
