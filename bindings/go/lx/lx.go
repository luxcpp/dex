// Package lx provides Go bindings for the LX full stack.
//
// LX is a high-performance decentralized exchange engine with:
//   - LXPool (LP-9010): Uniswap v4-style AMM
//   - LXOracle (LP-9011): Multi-source price aggregation
//   - LXRouter (LP-9012): Optimized swap routing
//   - LXHooks (LP-9013): Hook contract registry
//   - LXFlash (LP-9014): Flash loan facility
//   - LXBook (LP-9020): CLOB matching engine
//   - LXVault (LP-9030): Margin/custody clearinghouse
//   - LXFeed (LP-9040): Mark/funding price feeds
package lx

/*
#cgo CFLAGS: -I${SRCDIR}/../../../include -I${SRCDIR}/../../c
#cgo LDFLAGS: -L${SRCDIR}/../../../build -lluxdex_c -lstdc++

#include "lx_full_c.h"
#include <stdlib.h>
*/
import "C"
import (
	"errors"
	"runtime"
	"unsafe"
)

// =============================================================================
// Constants
// =============================================================================

// X18One is 1.0 in X18 fixed-point (1e18)
const X18One int64 = 1_000_000_000_000_000_000

// AddressSize is the byte length of an address (20 bytes)
const AddressSize = 20

// Error codes
var (
	ErrOK                     = errors.New("ok")
	ErrPoolNotInitialized     = errors.New("pool not initialized")
	ErrPoolAlreadyInitialized = errors.New("pool already initialized")
	ErrInvalidTickRange       = errors.New("invalid tick range")
	ErrInsufficientLiquidity  = errors.New("insufficient liquidity")
	ErrPriceLimitExceeded     = errors.New("price limit exceeded")
	ErrInvalidCurrency        = errors.New("invalid currency")
	ErrCurrenciesNotSorted    = errors.New("currencies not sorted")
	ErrInvalidFee             = errors.New("invalid fee")
	ErrInsufficientBalance    = errors.New("insufficient balance")
	ErrInsufficientMargin     = errors.New("insufficient margin")
	ErrPositionNotFound       = errors.New("position not found")
	ErrOrderNotFound          = errors.New("order not found")
	ErrMarketNotFound         = errors.New("market not found")
)

// Fee tiers (in hundredths of a bip)
const (
	Fee001 uint32 = 100   // 0.01%
	Fee005 uint32 = 500   // 0.05%
	Fee030 uint32 = 3000  // 0.30%
	Fee100 uint32 = 10000 // 1.00%
)

// =============================================================================
// Types
// =============================================================================

// Address is a 20-byte Ethereum-style address.
type Address [AddressSize]byte

// Currency represents a token address.
type Currency = Address

// X18 is a 128-bit fixed-point number with 18 decimal places.
type X18 struct {
	Lo int64
	Hi int64
}

// Account identifies a trading account (main address + subaccount).
type Account struct {
	Main         Address
	SubaccountID uint16
}

// PoolKey uniquely identifies an AMM pool.
type PoolKey struct {
	Currency0   Currency
	Currency1   Currency
	Fee         uint32
	TickSpacing int32
	Hooks       Address
}

// BalanceDelta represents signed token amount changes.
type BalanceDelta struct {
	Amount0 X18
	Amount1 X18
}

// SwapParams configures a swap operation.
type SwapParams struct {
	ZeroForOne      bool // true = sell token0 for token1
	AmountSpecified X18  // positive = exact input, negative = exact output
	SqrtPriceLimit  X18  // price limit (0 = no limit)
}

// ModifyLiquidityParams configures a liquidity modification.
type ModifyLiquidityParams struct {
	TickLower      int32
	TickUpper      int32
	LiquidityDelta X18 // positive = add, negative = remove
	Salt           uint64
}

// TIF is the time-in-force for an order.
type TIF uint8

const (
	TifGTC TIF = 0 // Good Till Cancel
	TifIOC TIF = 1 // Immediate Or Cancel
	TifALO TIF = 2 // Add Liquidity Only (post-only)
)

// OrderKind is the type of order.
type OrderKind uint8

const (
	OrderLimit      OrderKind = 0
	OrderMarket     OrderKind = 1
	OrderStopMarket OrderKind = 2
	OrderStopLimit  OrderKind = 3
	OrderTakeMarket OrderKind = 4
	OrderTakeLimit  OrderKind = 5
)

// MarginMode is the margin mode for a position.
type MarginMode uint8

const (
	MarginCross    MarginMode = 0
	MarginIsolated MarginMode = 1
)

// PositionSide is the direction of a position.
type PositionSide uint8

const (
	PositionLong  PositionSide = 0
	PositionShort PositionSide = 1
)

// OrderStatus is the status of an order.
type OrderStatus uint8

const (
	StatusNew       OrderStatus = 0
	StatusOpen      OrderStatus = 1
	StatusFilled    OrderStatus = 2
	StatusCancelled OrderStatus = 3
	StatusRejected  OrderStatus = 4
)

// PriceType is the type of price.
type PriceType uint8

const (
	PriceIndex PriceType = 0
	PriceMark  PriceType = 1
	PriceLast  PriceType = 2
	PriceMid   PriceType = 3
)

// PriceSource identifies the source of a price.
type PriceSource uint8

const (
	SourceBinance   PriceSource = 0
	SourceCoinbase  PriceSource = 1
	SourceOKX       PriceSource = 2
	SourceBybit     PriceSource = 3
	SourceUniswap   PriceSource = 4
	SourceLXPool    PriceSource = 5
	SourceChainlink PriceSource = 6
	SourcePyth      PriceSource = 7
)

// Order represents an order to place on the CLOB.
type Order struct {
	MarketID     uint32
	IsBuy        bool
	Kind         OrderKind
	SizeX18      X18
	LimitPxX18   X18
	TriggerPxX18 X18
	ReduceOnly   bool
	TIF          TIF
	CLOID        [16]byte // Client order ID (UUID)
}

// PlaceResult is the result of placing an order.
type PlaceResult struct {
	OID           uint64
	Status        OrderStatus
	FilledSizeX18 X18
	AvgPxX18      X18
}

// L1 is Level-1 market data (best bid/ask).
type L1 struct {
	BestBidPxX18   X18
	BestBidSzX18   X18
	BestAskPxX18   X18
	BestAskSzX18   X18
	LastTradePxX18 X18
}

// Position represents an open position.
type Position struct {
	MarketID              uint32
	Side                  PositionSide
	SizeX18               X18
	EntryPxX18            X18
	UnrealizedPnlX18      X18
	AccumulatedFundingX18 X18
	LastFundingTime       uint64
}

// MarginInfo contains margin information for an account.
type MarginInfo struct {
	TotalCollateralX18   X18
	UsedMarginX18        X18
	FreeMarginX18        X18
	MarginRatioX18       X18
	MaintenanceMarginX18 X18
	Liquidatable         bool
}

// MarkPrice contains mark price information.
type MarkPrice struct {
	IndexPxX18 X18
	MarkPxX18  X18
	PremiumX18 X18
	Timestamp  uint64
}

// FundingRate contains funding rate information.
type FundingRate struct {
	RateX18         X18
	NextFundingTime uint64
}

// MarketConfig configures a perpetual market for the vault.
type MarketConfig struct {
	MarketID             uint32
	BaseCurrency         Currency
	QuoteCurrency        Currency
	InitialMarginX18     X18
	MaintenanceMarginX18 X18
	MaxLeverageX18       X18
	TakerFeeX18          X18
	MakerFeeX18          X18
	MinOrderSizeX18      X18
	MaxPositionSizeX18   X18
	ReduceOnlyMode       bool
	Active               bool
}

// BookMarketConfig configures a market for the order book.
type BookMarketConfig struct {
	MarketID        uint32
	SymbolID        uint64
	BaseCurrency    Currency
	QuoteCurrency   Currency
	TickSizeX18     X18
	LotSizeX18      X18
	MinNotionalX18  X18
	MaxOrderSizeX18 X18
	PostOnlyMode    bool
	ReduceOnlyMode  bool
	Status          uint8
}

// GlobalStats contains global DEX statistics.
type GlobalStats struct {
	PoolTotalPools        uint64
	PoolTotalSwaps        uint64
	BookTotalMarkets      uint64
	BookTotalOrdersPlaced uint64
	BookTotalTrades       uint64
	VaultTotalAccounts    uint64
	VaultTotalPositions   uint64
	OracleTotalAssets     uint64
	OracleTotalUpdates    uint64
	FeedTotalMarkets      uint64
	UptimeSeconds         uint64
}

// =============================================================================
// LP-Aligned Precompile Addresses
// =============================================================================

var (
	// AMM Core (LP-9010 series)
	LXPoolAddress   = Address{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x10}
	LXOracleAddress = Address{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x11}
	LXRouterAddress = Address{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x12}
	LXHooksAddress  = Address{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x13}
	LXFlashAddress  = Address{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x14}

	// CLOB (LP-9020 series)
	LXBookAddress = Address{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x20}

	// Custody/Margin (LP-9030 series)
	LXVaultAddress = Address{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x30}

	// Price Feeds (LP-9040 series)
	LXFeedAddress = Address{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x40}

	// Lending (LP-9050 series)
	LXLendAddress = Address{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x50}

	// Self-Repaying Loans (LP-9060 series)
	LXLiquidAddress = Address{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x60}
)

// AddressFromLP creates an address from an LP number.
func AddressFromLP(lp uint16) Address {
	var addr Address
	addr[18] = byte(lp >> 8)
	addr[19] = byte(lp & 0xFF)
	return addr
}

// ToLP extracts the LP number from an address.
func (a Address) ToLP() uint16 {
	return uint16(a[18])<<8 | uint16(a[19])
}

// IsDEXPrecompile checks if the address is a DEX precompile (LP-9xxx).
func (a Address) IsDEXPrecompile() bool {
	for i := 0; i < 17; i++ {
		if a[i] != 0 {
			return false
		}
	}
	return a[17] == 0 && (a[18]&0xF0) == 0x90
}

// =============================================================================
// X18 Arithmetic
// =============================================================================

// X18Zero returns zero.
func X18Zero() X18 {
	return X18{Lo: 0, Hi: 0}
}

// X18FromInt creates an X18 from an integer.
func X18FromInt(v int64) X18 {
	return fromCX18(C.lx_from_int(C.int64_t(v)))
}

// X18FromFloat creates an X18 from a float64.
func X18FromFloat(v float64) X18 {
	return fromCX18(C.lx_from_double(C.double(v)))
}

// ToInt converts X18 to int64 (truncates decimals).
func (x X18) ToInt() int64 {
	return int64(C.lx_to_int(toCX18(x)))
}

// ToFloat converts X18 to float64.
func (x X18) ToFloat() float64 {
	return float64(C.lx_to_double(toCX18(x)))
}

// IsZero returns true if the value is zero.
func (x X18) IsZero() bool {
	return x.Lo == 0 && x.Hi == 0
}

// IsNegative returns true if the value is negative.
func (x X18) IsNegative() bool {
	return x.Hi < 0
}

// =============================================================================
// LX Controller
// =============================================================================

// LX is the main DEX controller.
type LX struct {
	ptr C.LxHandle
}

// New creates a new LX instance.
func New() (*LX, error) {
	ptr := C.lx_create()
	if ptr == nil {
		return nil, errors.New("failed to create LX instance")
	}
	dex := &LX{ptr: ptr}
	runtime.SetFinalizer(dex, (*LX).Close)
	return dex, nil
}

// Close releases the LX resources.
func (d *LX) Close() {
	if d.ptr != nil {
		C.lx_destroy(d.ptr)
		d.ptr = nil
	}
	runtime.SetFinalizer(d, nil)
}

// Initialize initializes the DEX.
func (d *LX) Initialize() {
	if d.ptr != nil {
		C.lx_initialize(d.ptr)
	}
}

// Start starts the DEX.
func (d *LX) Start() {
	if d.ptr != nil {
		C.lx_start(d.ptr)
	}
}

// Stop stops the DEX.
func (d *LX) Stop() {
	if d.ptr != nil {
		C.lx_stop(d.ptr)
	}
}

// IsRunning returns true if the DEX is running.
func (d *LX) IsRunning() bool {
	if d.ptr == nil {
		return false
	}
	return bool(C.lx_is_running(d.ptr))
}

// Version returns the LX version string.
func Version() string {
	return C.GoString(C.lx_version())
}

// GetStats returns global DEX statistics.
func (d *LX) GetStats() GlobalStats {
	if d.ptr == nil {
		return GlobalStats{}
	}
	cs := C.lx_get_stats(d.ptr)
	return GlobalStats{
		PoolTotalPools:        uint64(cs.pool_total_pools),
		PoolTotalSwaps:        uint64(cs.pool_total_swaps),
		BookTotalMarkets:      uint64(cs.book_total_markets),
		BookTotalOrdersPlaced: uint64(cs.book_total_orders_placed),
		BookTotalTrades:       uint64(cs.book_total_trades),
		VaultTotalAccounts:    uint64(cs.vault_total_accounts),
		VaultTotalPositions:   uint64(cs.vault_total_positions),
		OracleTotalAssets:     uint64(cs.oracle_total_assets),
		OracleTotalUpdates:    uint64(cs.oracle_total_updates),
		FeedTotalMarkets:      uint64(cs.feed_total_markets),
		UptimeSeconds:         uint64(cs.uptime_seconds),
	}
}

// =============================================================================
// Pool Operations (LP-9010)
// =============================================================================

// PoolInitialize initializes a new AMM pool.
func (d *LX) PoolInitialize(key PoolKey, sqrtPriceX96 X18) (int32, error) {
	if d.ptr == nil {
		return 0, errors.New("LX not initialized")
	}
	cKey := toCPoolKey(key)
	result := int32(C.lx_pool_initialize(d.ptr, &cKey, toCX18(sqrtPriceX96)))
	return result, errorFromCode(result)
}

// PoolSwap executes a swap on an AMM pool.
func (d *LX) PoolSwap(key PoolKey, params SwapParams) (BalanceDelta, error) {
	if d.ptr == nil {
		return BalanceDelta{}, errors.New("LX not initialized")
	}
	cKey := toCPoolKey(key)
	cParams := toCSwapParams(params)
	result := C.lx_pool_swap(d.ptr, &cKey, &cParams)
	return fromCBalanceDelta(result), nil
}

// PoolModifyLiquidity adds or removes liquidity from a pool.
func (d *LX) PoolModifyLiquidity(key PoolKey, params ModifyLiquidityParams) (BalanceDelta, error) {
	if d.ptr == nil {
		return BalanceDelta{}, errors.New("LX not initialized")
	}
	cKey := toCPoolKey(key)
	cParams := toCModifyLiquidityParams(params)
	var result C.LxBalanceDelta
	if params.LiquidityDelta.IsNegative() {
		result = C.lx_pool_remove_liquidity(d.ptr, &cKey, &cParams)
	} else {
		result = C.lx_pool_add_liquidity(d.ptr, &cKey, &cParams)
	}
	return fromCBalanceDelta(result), nil
}

// PoolExists checks if a pool exists.
func (d *LX) PoolExists(key PoolKey) bool {
	if d.ptr == nil {
		return false
	}
	cKey := toCPoolKey(key)
	return bool(C.lx_pool_exists(d.ptr, &cKey))
}

// PoolGetLiquidity returns the total liquidity in a pool.
func (d *LX) PoolGetLiquidity(key PoolKey) X18 {
	if d.ptr == nil {
		return X18Zero()
	}
	cKey := toCPoolKey(key)
	return fromCX18(C.lx_pool_get_liquidity(d.ptr, &cKey))
}

// =============================================================================
// Book Operations (LP-9020)
// =============================================================================

// BookCreateMarket creates a new order book market.
func (d *LX) BookCreateMarket(config BookMarketConfig) error {
	if d.ptr == nil {
		return errors.New("LX not initialized")
	}
	cConfig := toCBookMarketConfig(config)
	result := int32(C.lx_book_create_market(d.ptr, &cConfig))
	return errorFromCode(result)
}

// BookPlaceOrder places an order on the order book.
func (d *LX) BookPlaceOrder(sender Account, order Order) (PlaceResult, error) {
	if d.ptr == nil {
		return PlaceResult{}, errors.New("LX not initialized")
	}
	cAccount := toCAccount(sender)
	cOrder := toCOrder(order)
	cResult := C.lx_book_place_order(d.ptr, &cAccount, &cOrder)
	return fromCPlaceResult(cResult), nil
}

// BookCancelOrder cancels an order by order ID.
func (d *LX) BookCancelOrder(sender Account, marketID uint32, oid uint64) error {
	if d.ptr == nil {
		return errors.New("LX not initialized")
	}
	cAccount := toCAccount(sender)
	result := int32(C.lx_book_cancel_order(d.ptr, &cAccount, C.uint32_t(marketID), C.uint64_t(oid)))
	return errorFromCode(result)
}

// BookCancelByCLOID cancels an order by client order ID.
func (d *LX) BookCancelByCLOID(sender Account, marketID uint32, cloid [16]byte) error {
	if d.ptr == nil {
		return errors.New("LX not initialized")
	}
	cAccount := toCAccount(sender)
	result := int32(C.lx_book_cancel_by_cloid(d.ptr, &cAccount, C.uint32_t(marketID), (*C.uint8_t)(&cloid[0])))
	return errorFromCode(result)
}

// BookCancelAll cancels all orders for an account in a market.
func (d *LX) BookCancelAll(sender Account, marketID uint32) error {
	if d.ptr == nil {
		return errors.New("LX not initialized")
	}
	cAccount := toCAccount(sender)
	result := int32(C.lx_book_cancel_all(d.ptr, &cAccount, C.uint32_t(marketID)))
	return errorFromCode(result)
}

// BookGetL1 returns Level-1 market data.
func (d *LX) BookGetL1(marketID uint32) L1 {
	if d.ptr == nil {
		return L1{}
	}
	cL1 := C.lx_book_get_l1(d.ptr, C.uint32_t(marketID))
	return fromCL1(cL1)
}

// BookMarketExists checks if a market exists.
func (d *LX) BookMarketExists(marketID uint32) bool {
	if d.ptr == nil {
		return false
	}
	return bool(C.lx_book_market_exists(d.ptr, C.uint32_t(marketID)))
}

// =============================================================================
// Vault Operations (LP-9030)
// =============================================================================

// VaultCreateMarket creates a new margin market.
func (d *LX) VaultCreateMarket(config MarketConfig) error {
	if d.ptr == nil {
		return errors.New("LX not initialized")
	}
	cConfig := toCMarketConfig(config)
	result := int32(C.lx_vault_create_market(d.ptr, &cConfig))
	return errorFromCode(result)
}

// VaultDeposit deposits tokens into the vault.
func (d *LX) VaultDeposit(account Account, token Currency, amount X18) error {
	if d.ptr == nil {
		return errors.New("LX not initialized")
	}
	cAccount := toCAccount(account)
	cToken := toCCurrency(token)
	result := int32(C.lx_vault_deposit(d.ptr, &cAccount, &cToken, toCX18(amount)))
	return errorFromCode(result)
}

// VaultWithdraw withdraws tokens from the vault.
func (d *LX) VaultWithdraw(account Account, token Currency, amount X18) error {
	if d.ptr == nil {
		return errors.New("LX not initialized")
	}
	cAccount := toCAccount(account)
	cToken := toCCurrency(token)
	result := int32(C.lx_vault_withdraw(d.ptr, &cAccount, &cToken, toCX18(amount)))
	return errorFromCode(result)
}

// VaultGetBalance returns the balance of a token for an account.
func (d *LX) VaultGetBalance(account Account, token Currency) X18 {
	if d.ptr == nil {
		return X18Zero()
	}
	cAccount := toCAccount(account)
	cToken := toCCurrency(token)
	return fromCX18(C.lx_vault_get_balance(d.ptr, &cAccount, &cToken))
}

// VaultGetPosition returns a position for an account.
func (d *LX) VaultGetPosition(account Account, marketID uint32) (*Position, bool) {
	if d.ptr == nil {
		return nil, false
	}
	cAccount := toCAccount(account)
	var cPos C.LxPosition
	if !C.lx_vault_get_position(d.ptr, &cAccount, C.uint32_t(marketID), &cPos) {
		return nil, false
	}
	pos := fromCPosition(cPos)
	return &pos, true
}

// VaultGetMargin returns margin information for an account.
func (d *LX) VaultGetMargin(account Account) MarginInfo {
	if d.ptr == nil {
		return MarginInfo{}
	}
	cAccount := toCAccount(account)
	cInfo := C.lx_vault_get_margin_info(d.ptr, &cAccount)
	return fromCMarginInfo(cInfo)
}

// VaultIsLiquidatable checks if an account is liquidatable.
func (d *LX) VaultIsLiquidatable(account Account) bool {
	if d.ptr == nil {
		return false
	}
	cAccount := toCAccount(account)
	return bool(C.lx_vault_is_liquidatable(d.ptr, &cAccount))
}

// VaultAccrueFunding accrues funding for a market.
func (d *LX) VaultAccrueFunding(marketID uint32) error {
	if d.ptr == nil {
		return errors.New("LX not initialized")
	}
	result := int32(C.lx_vault_accrue_funding(d.ptr, C.uint32_t(marketID)))
	return errorFromCode(result)
}

// =============================================================================
// Oracle Operations (LP-9011)
// =============================================================================

// OracleRegisterAsset registers a new asset with the oracle.
func (d *LX) OracleRegisterAsset(assetID uint64) error {
	if d.ptr == nil {
		return errors.New("LX not initialized")
	}
	result := int32(C.lx_oracle_register_asset(d.ptr, C.uint64_t(assetID)))
	return errorFromCode(result)
}

// OracleUpdatePrice updates the price for an asset.
func (d *LX) OracleUpdatePrice(assetID uint64, source PriceSource, price X18, confidence X18) error {
	if d.ptr == nil {
		return errors.New("LX not initialized")
	}
	result := int32(C.lx_oracle_update_price(d.ptr, C.uint64_t(assetID),
		C.LxPriceSource(source), toCX18(price), toCX18(confidence)))
	return errorFromCode(result)
}

// OracleGetPrice returns the aggregated price for an asset.
func (d *LX) OracleGetPrice(assetID uint64) (X18, error) {
	if d.ptr == nil {
		return X18Zero(), errors.New("LX not initialized")
	}
	var cPrice C.LxI128
	if !C.lx_oracle_get_price(d.ptr, C.uint64_t(assetID), &cPrice) {
		return X18Zero(), ErrMarketNotFound
	}
	return fromCX18(cPrice), nil
}

// OracleIsPriceFresh checks if the price is fresh.
func (d *LX) OracleIsPriceFresh(assetID uint64) bool {
	if d.ptr == nil {
		return false
	}
	return bool(C.lx_oracle_is_price_fresh(d.ptr, C.uint64_t(assetID)))
}

// OraclePriceAge returns the age of the price in seconds.
func (d *LX) OraclePriceAge(assetID uint64) uint64 {
	if d.ptr == nil {
		return 0
	}
	return uint64(C.lx_oracle_price_age(d.ptr, C.uint64_t(assetID)))
}

// =============================================================================
// Feed Operations (LP-9040)
// =============================================================================

// FeedRegisterMarket registers a market with the price feed.
func (d *LX) FeedRegisterMarket(marketID uint32, assetID uint64) error {
	if d.ptr == nil {
		return errors.New("LX not initialized")
	}
	result := int32(C.lx_feed_register_market(d.ptr, C.uint32_t(marketID), C.uint64_t(assetID)))
	return errorFromCode(result)
}

// FeedGetIndexPrice returns the index price for a market.
func (d *LX) FeedGetIndexPrice(marketID uint32) (X18, error) {
	if d.ptr == nil {
		return X18Zero(), errors.New("LX not initialized")
	}
	var cPrice C.LxI128
	if !C.lx_feed_get_index_price(d.ptr, C.uint32_t(marketID), &cPrice) {
		return X18Zero(), ErrMarketNotFound
	}
	return fromCX18(cPrice), nil
}

// FeedGetMarkPrice returns the mark price for a market.
func (d *LX) FeedGetMarkPrice(marketID uint32) (MarkPrice, error) {
	if d.ptr == nil {
		return MarkPrice{}, errors.New("LX not initialized")
	}
	var cMP C.LxMarkPrice
	if !C.lx_feed_get_mark_price(d.ptr, C.uint32_t(marketID), &cMP) {
		return MarkPrice{}, ErrMarketNotFound
	}
	return fromCMarkPrice(cMP), nil
}

// FeedGetLastPrice returns the last trade price for a market.
func (d *LX) FeedGetLastPrice(marketID uint32) (X18, error) {
	if d.ptr == nil {
		return X18Zero(), errors.New("LX not initialized")
	}
	var cPrice C.LxI128
	if !C.lx_feed_get_last_price(d.ptr, C.uint32_t(marketID), &cPrice) {
		return X18Zero(), ErrMarketNotFound
	}
	return fromCX18(cPrice), nil
}

// FeedGetMidPrice returns the mid price for a market.
func (d *LX) FeedGetMidPrice(marketID uint32) (X18, error) {
	if d.ptr == nil {
		return X18Zero(), errors.New("LX not initialized")
	}
	var cPrice C.LxI128
	if !C.lx_feed_get_mid_price(d.ptr, C.uint32_t(marketID), &cPrice) {
		return X18Zero(), ErrMarketNotFound
	}
	return fromCX18(cPrice), nil
}

// FeedGetFundingRate returns the funding rate for a market.
func (d *LX) FeedGetFundingRate(marketID uint32) (FundingRate, error) {
	if d.ptr == nil {
		return FundingRate{}, errors.New("LX not initialized")
	}
	var cFR C.LxFundingRate
	if !C.lx_feed_get_funding_rate(d.ptr, C.uint32_t(marketID), &cFR) {
		return FundingRate{}, ErrMarketNotFound
	}
	return fromCFundingRate(cFR), nil
}

// FeedUpdateLastPrice updates the last trade price.
func (d *LX) FeedUpdateLastPrice(marketID uint32, price X18) {
	if d.ptr != nil {
		C.lx_feed_update_last_price(d.ptr, C.uint32_t(marketID), toCX18(price))
	}
}

// FeedUpdateBBO updates the best bid/offer.
func (d *LX) FeedUpdateBBO(marketID uint32, bestBid, bestAsk X18) {
	if d.ptr != nil {
		C.lx_feed_update_bbo(d.ptr, C.uint32_t(marketID), toCX18(bestBid), toCX18(bestAsk))
	}
}

// FeedCalculateFundingRate calculates the funding rate for a market.
func (d *LX) FeedCalculateFundingRate(marketID uint32) {
	if d.ptr != nil {
		C.lx_feed_calculate_funding_rate(d.ptr, C.uint32_t(marketID))
	}
}

// =============================================================================
// Precompile Router
// =============================================================================

// PrecompileCall calls a precompile with the given calldata.
func (d *LX) PrecompileCall(precompile Address, calldata []byte) ([]byte, error) {
	if d.ptr == nil {
		return nil, errors.New("LX not initialized")
	}

	cAddr := toCAddress(precompile)
	var calldataPtr *C.uint8_t
	if len(calldata) > 0 {
		calldataPtr = (*C.uint8_t)(unsafe.Pointer(&calldata[0]))
	}

	// First call to get the result size
	resultSize := C.lx_precompile_call(d.ptr, &cAddr, calldataPtr, C.size_t(len(calldata)), nil, 0)
	if resultSize == 0 {
		return nil, nil
	}

	// Allocate and call again
	result := make([]byte, resultSize)
	C.lx_precompile_call(d.ptr, &cAddr, calldataPtr, C.size_t(len(calldata)),
		(*C.uint8_t)(unsafe.Pointer(&result[0])), C.size_t(len(result)))

	return result, nil
}

// IsPrecompile checks if the address is a DEX precompile.
func IsPrecompile(addr Address) bool {
	cAddr := toCAddress(addr)
	return bool(C.lx_is_precompile(&cAddr))
}

// PrecompileGasCost returns the gas cost for a precompile call.
func (d *LX) PrecompileGasCost(precompile Address, calldata []byte) uint64 {
	if d.ptr == nil {
		return 0
	}

	cAddr := toCAddress(precompile)
	var calldataPtr *C.uint8_t
	if len(calldata) > 0 {
		calldataPtr = (*C.uint8_t)(unsafe.Pointer(&calldata[0]))
	}

	return uint64(C.lx_precompile_gas_cost(d.ptr, &cAddr, calldataPtr, C.size_t(len(calldata))))
}

// =============================================================================
// Conversion Helpers
// =============================================================================

func toCX18(x X18) C.LxI128 {
	return C.LxI128{lo: C.int64_t(x.Lo), hi: C.int64_t(x.Hi)}
}

func fromCX18(c C.LxI128) X18 {
	return X18{Lo: int64(c.lo), Hi: int64(c.hi)}
}

func toCAddress(a Address) C.LxAddress {
	var ca C.LxAddress
	for i := 0; i < AddressSize; i++ {
		ca.bytes[i] = C.uint8_t(a[i])
	}
	return ca
}

func fromCAddress(c C.LxAddress) Address {
	var a Address
	for i := 0; i < AddressSize; i++ {
		a[i] = byte(c.bytes[i])
	}
	return a
}

func toCCurrency(c Currency) C.LxCurrency {
	return toCAddress(c)
}

func toCAccount(a Account) C.LxAccount {
	return C.LxAccount{
		main:          toCAddress(a.Main),
		subaccount_id: C.uint16_t(a.SubaccountID),
	}
}

func toCPoolKey(k PoolKey) C.LxPoolKey {
	return C.LxPoolKey{
		currency0:    toCCurrency(k.Currency0),
		currency1:    toCCurrency(k.Currency1),
		fee:          C.uint32_t(k.Fee),
		tick_spacing: C.int32_t(k.TickSpacing),
		hooks:        toCAddress(k.Hooks),
	}
}

func toCSwapParams(p SwapParams) C.LxSwapParams {
	return C.LxSwapParams{
		zero_for_one:     C.bool(p.ZeroForOne),
		amount_specified: toCX18(p.AmountSpecified),
		sqrt_price_limit: toCX18(p.SqrtPriceLimit),
	}
}

func toCModifyLiquidityParams(p ModifyLiquidityParams) C.LxModifyLiquidityParams {
	return C.LxModifyLiquidityParams{
		tick_lower:      C.int32_t(p.TickLower),
		tick_upper:      C.int32_t(p.TickUpper),
		liquidity_delta: toCX18(p.LiquidityDelta),
		salt:            C.uint64_t(p.Salt),
	}
}

func toCOrder(o Order) C.LxOrder {
	co := C.LxOrder{
		market_id:      C.uint32_t(o.MarketID),
		is_buy:         C.bool(o.IsBuy),
		kind:           C.LxOrderKind(o.Kind),
		size_x18:       toCX18(o.SizeX18),
		limit_px_x18:   toCX18(o.LimitPxX18),
		trigger_px_x18: toCX18(o.TriggerPxX18),
		reduce_only:    C.bool(o.ReduceOnly),
		tif:            C.LxTIF(o.TIF),
	}
	for i := 0; i < 16; i++ {
		co.cloid[i] = C.uint8_t(o.CLOID[i])
	}
	return co
}

func toCMarketConfig(c MarketConfig) C.LxMarketConfig {
	return C.LxMarketConfig{
		market_id:              C.uint32_t(c.MarketID),
		base_currency:          toCCurrency(c.BaseCurrency),
		quote_currency:         toCCurrency(c.QuoteCurrency),
		initial_margin_x18:     toCX18(c.InitialMarginX18),
		maintenance_margin_x18: toCX18(c.MaintenanceMarginX18),
		max_leverage_x18:       toCX18(c.MaxLeverageX18),
		taker_fee_x18:          toCX18(c.TakerFeeX18),
		maker_fee_x18:          toCX18(c.MakerFeeX18),
		min_order_size_x18:     toCX18(c.MinOrderSizeX18),
		max_position_size_x18:  toCX18(c.MaxPositionSizeX18),
		reduce_only_mode:       C.bool(c.ReduceOnlyMode),
		active:                 C.bool(c.Active),
	}
}

func toCBookMarketConfig(c BookMarketConfig) C.LxBookMarketConfig {
	return C.LxBookMarketConfig{
		market_id:          C.uint32_t(c.MarketID),
		symbol_id:          C.uint64_t(c.SymbolID),
		base_currency:      toCCurrency(c.BaseCurrency),
		quote_currency:     toCCurrency(c.QuoteCurrency),
		tick_size_x18:      toCX18(c.TickSizeX18),
		lot_size_x18:       toCX18(c.LotSizeX18),
		min_notional_x18:   toCX18(c.MinNotionalX18),
		max_order_size_x18: toCX18(c.MaxOrderSizeX18),
		post_only_mode:     C.bool(c.PostOnlyMode),
		reduce_only_mode:   C.bool(c.ReduceOnlyMode),
		status:             C.uint8_t(c.Status),
	}
}

func fromCBalanceDelta(c C.LxBalanceDelta) BalanceDelta {
	return BalanceDelta{
		Amount0: fromCX18(c.amount0),
		Amount1: fromCX18(c.amount1),
	}
}

func fromCPlaceResult(c C.LxPlaceResult) PlaceResult {
	return PlaceResult{
		OID:           uint64(c.oid),
		Status:        OrderStatus(c.status),
		FilledSizeX18: fromCX18(c.filled_size_x18),
		AvgPxX18:      fromCX18(c.avg_px_x18),
	}
}

func fromCL1(c C.LxL1) L1 {
	return L1{
		BestBidPxX18:   fromCX18(c.best_bid_px_x18),
		BestBidSzX18:   fromCX18(c.best_bid_sz_x18),
		BestAskPxX18:   fromCX18(c.best_ask_px_x18),
		BestAskSzX18:   fromCX18(c.best_ask_sz_x18),
		LastTradePxX18: fromCX18(c.last_trade_px_x18),
	}
}

func fromCPosition(c C.LxPosition) Position {
	return Position{
		MarketID:              uint32(c.market_id),
		Side:                  PositionSide(c.side),
		SizeX18:               fromCX18(c.size_x18),
		EntryPxX18:            fromCX18(c.entry_px_x18),
		UnrealizedPnlX18:      fromCX18(c.unrealized_pnl_x18),
		AccumulatedFundingX18: fromCX18(c.accumulated_funding_x18),
		LastFundingTime:       uint64(c.last_funding_time),
	}
}

func fromCMarginInfo(c C.LxMarginInfo) MarginInfo {
	return MarginInfo{
		TotalCollateralX18:   fromCX18(c.total_collateral_x18),
		UsedMarginX18:        fromCX18(c.used_margin_x18),
		FreeMarginX18:        fromCX18(c.free_margin_x18),
		MarginRatioX18:       fromCX18(c.margin_ratio_x18),
		MaintenanceMarginX18: fromCX18(c.maintenance_margin_x18),
		Liquidatable:         bool(c.liquidatable),
	}
}

func fromCMarkPrice(c C.LxMarkPrice) MarkPrice {
	return MarkPrice{
		IndexPxX18: fromCX18(c.index_px_x18),
		MarkPxX18:  fromCX18(c.mark_px_x18),
		PremiumX18: fromCX18(c.premium_x18),
		Timestamp:  uint64(c.timestamp),
	}
}

func fromCFundingRate(c C.LxFundingRate) FundingRate {
	return FundingRate{
		RateX18:         fromCX18(c.rate_x18),
		NextFundingTime: uint64(c.next_funding_time),
	}
}

func errorFromCode(code int32) error {
	switch code {
	case 0:
		return nil
	case -1:
		return ErrPoolNotInitialized
	case -2:
		return ErrPoolAlreadyInitialized
	case -3:
		return ErrInvalidTickRange
	case -4:
		return ErrInsufficientLiquidity
	case -5:
		return ErrPriceLimitExceeded
	case -6:
		return ErrInvalidCurrency
	case -7:
		return ErrCurrenciesNotSorted
	case -8:
		return ErrInvalidFee
	case -10:
		return ErrInsufficientBalance
	case -11:
		return ErrInsufficientMargin
	case -12:
		return ErrPositionNotFound
	case -13:
		return ErrOrderNotFound
	case -14:
		return ErrMarketNotFound
	default:
		return errors.New("unknown error")
	}
}
