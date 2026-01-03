// Package luxdex provides Go bindings for the Lux DEX matching engine.
package luxdex

import (
	"errors"
	"sync"
	"time"
)

// Price multiplier for fixed-point arithmetic (1e8)
const PriceMultiplier = 100_000_000

// Side represents the order side (buy/sell)
type Side uint8

const (
	SideBuy  Side = 0
	SideSell Side = 1
)

func (s Side) String() string {
	if s == SideBuy {
		return "buy"
	}
	return "sell"
}

// OrderType represents the order type
type OrderType uint8

const (
	OrderTypeLimit     OrderType = 0
	OrderTypeMarket    OrderType = 1
	OrderTypeStop      OrderType = 2
	OrderTypeStopLimit OrderType = 3
)

func (t OrderType) String() string {
	switch t {
	case OrderTypeLimit:
		return "limit"
	case OrderTypeMarket:
		return "market"
	case OrderTypeStop:
		return "stop"
	case OrderTypeStopLimit:
		return "stop_limit"
	default:
		return "unknown"
	}
}

// TimeInForce represents the order time-in-force
type TimeInForce uint8

const (
	TifGTC TimeInForce = 0 // Good Till Cancel
	TifIOC TimeInForce = 1 // Immediate Or Cancel
	TifFOK TimeInForce = 2 // Fill Or Kill
	TifGTD TimeInForce = 3 // Good Till Date
	TifDAY TimeInForce = 4 // Day order
)

func (t TimeInForce) String() string {
	switch t {
	case TifGTC:
		return "GTC"
	case TifIOC:
		return "IOC"
	case TifFOK:
		return "FOK"
	case TifGTD:
		return "GTD"
	case TifDAY:
		return "DAY"
	default:
		return "unknown"
	}
}

// OrderStatus represents the order status
type OrderStatus uint8

const (
	StatusNew             OrderStatus = 0
	StatusPartiallyFilled OrderStatus = 1
	StatusFilled          OrderStatus = 2
	StatusCancelled       OrderStatus = 3
	StatusRejected        OrderStatus = 4
	StatusExpired         OrderStatus = 5
)

func (s OrderStatus) String() string {
	switch s {
	case StatusNew:
		return "new"
	case StatusPartiallyFilled:
		return "partial"
	case StatusFilled:
		return "filled"
	case StatusCancelled:
		return "cancelled"
	case StatusRejected:
		return "rejected"
	case StatusExpired:
		return "expired"
	default:
		return "unknown"
	}
}

// Price is a fixed-point price (actual_price * 1e8)
type Price int64

// ToFloat converts fixed-point price to float64
func (p Price) ToFloat() float64 {
	return float64(p) / PriceMultiplier
}

// PriceFromFloat converts float64 to fixed-point price
func PriceFromFloat(f float64) Price {
	return Price(f * PriceMultiplier)
}

// Quantity is a fixed-point quantity (actual_qty * 1e8)
type Quantity int64

// ToFloat converts fixed-point quantity to float64
func (q Quantity) ToFloat() float64 {
	return float64(q) / PriceMultiplier
}

// QuantityFromFloat converts float64 to fixed-point quantity
func QuantityFromFloat(f float64) Quantity {
	return Quantity(f * PriceMultiplier)
}

// Order represents an order in the matching engine
type Order struct {
	ID         uint64
	SymbolID   uint64
	AccountID  uint64
	Price      Price
	Quantity   Quantity
	Filled     Quantity
	Side       Side
	Type       OrderType
	TIF        TimeInForce
	Status     OrderStatus
	STPGroup   uint64 // Self-trade prevention group
	StopPrice  Price
	Timestamp  time.Time
	ExpireTime time.Time
}

// Remaining returns the unfilled quantity
func (o *Order) Remaining() Quantity {
	return o.Quantity - o.Filled
}

// IsBuy returns true if this is a buy order
func (o *Order) IsBuy() bool {
	return o.Side == SideBuy
}

// IsSell returns true if this is a sell order
func (o *Order) IsSell() bool {
	return o.Side == SideSell
}

// IsActive returns true if the order is active (new or partially filled)
func (o *Order) IsActive() bool {
	return o.Status == StatusNew || o.Status == StatusPartiallyFilled
}

// IsFilled returns true if the order is completely filled
func (o *Order) IsFilled() bool {
	return o.Remaining() == 0
}

// Trade represents an executed trade
type Trade struct {
	ID              uint64
	SymbolID        uint64
	BuyOrderID      uint64
	SellOrderID     uint64
	BuyerAccountID  uint64
	SellerAccountID uint64
	Price           Price
	Quantity        Quantity
	AggressorSide   Side
	Timestamp       time.Time
}

// DepthLevel represents a single price level in the order book
type DepthLevel struct {
	Price      float64
	Quantity   float64
	OrderCount int
}

// MarketDepth represents the current market depth
type MarketDepth struct {
	Bids      []DepthLevel
	Asks      []DepthLevel
	Timestamp time.Time
}

// OrderResult represents the result of placing an order
type OrderResult struct {
	Success bool
	OrderID uint64
	Error   string
	Trades  []Trade
}

// CancelResult represents the result of cancelling an order
type CancelResult struct {
	Success        bool
	CancelledOrder *Order
	Error          string
}

// EngineStats contains engine statistics
type EngineStats struct {
	TotalOrdersPlaced    uint64
	TotalOrdersCancelled uint64
	TotalTrades          uint64
	TotalVolume          uint64
}

// EngineConfig contains engine configuration
type EngineConfig struct {
	WorkerThreads       int
	MaxBatchSize        int
	EnableSelfTradePrev bool
	AsyncMode           bool
}

// DefaultEngineConfig returns a default engine configuration
func DefaultEngineConfig() EngineConfig {
	return EngineConfig{
		WorkerThreads:       1,
		MaxBatchSize:        1000,
		EnableSelfTradePrev: true,
		AsyncMode:           false,
	}
}

// TradeListener is called when trades occur
type TradeListener interface {
	OnTrade(trade Trade)
	OnOrderFilled(order Order)
	OnOrderPartiallyFilled(order Order, fillQty Quantity)
	OnOrderCancelled(order Order)
}

// Engine is the main trading engine interface
type Engine interface {
	// Start starts the engine
	Start()

	// Stop stops the engine
	Stop()

	// IsRunning returns true if the engine is running
	IsRunning() bool

	// AddSymbol adds a new tradeable symbol
	AddSymbol(symbolID uint64) bool

	// RemoveSymbol removes a symbol (must have no orders)
	RemoveSymbol(symbolID uint64) bool

	// HasSymbol checks if a symbol exists
	HasSymbol(symbolID uint64) bool

	// Symbols returns all registered symbols
	Symbols() []uint64

	// PlaceOrder places an order
	PlaceOrder(order Order) OrderResult

	// CancelOrder cancels an order
	CancelOrder(symbolID, orderID uint64) CancelResult

	// GetOrder retrieves an order
	GetOrder(symbolID, orderID uint64) (*Order, bool)

	// GetDepth returns market depth
	GetDepth(symbolID uint64, levels int) MarketDepth

	// BestBid returns the best bid price
	BestBid(symbolID uint64) (Price, bool)

	// BestAsk returns the best ask price
	BestAsk(symbolID uint64) (Price, bool)

	// GetStats returns engine statistics
	GetStats() EngineStats

	// SetTradeListener sets the trade listener
	SetTradeListener(listener TradeListener)
}

// OrderIDGenerator generates unique order IDs
type OrderIDGenerator struct {
	mu      sync.Mutex
	counter uint64
}

var globalOrderIDGen = &OrderIDGenerator{counter: 1}

// NextOrderID generates the next order ID
func NextOrderID() uint64 {
	globalOrderIDGen.mu.Lock()
	defer globalOrderIDGen.mu.Unlock()
	id := globalOrderIDGen.counter
	globalOrderIDGen.counter++
	return id
}

// ResetOrderIDGenerator resets the order ID generator
func ResetOrderIDGenerator(start uint64) {
	globalOrderIDGen.mu.Lock()
	defer globalOrderIDGen.mu.Unlock()
	globalOrderIDGen.counter = start
}

// Common errors
var (
	ErrUnknownSymbol  = errors.New("unknown symbol")
	ErrOrderNotFound  = errors.New("order not found")
	ErrInvalidOrder   = errors.New("invalid order")
	ErrEngineNotReady = errors.New("engine not ready")
)

// OrderBuilder helps construct orders
type OrderBuilder struct {
	order Order
}

// NewOrder creates a new order builder
func NewOrder() *OrderBuilder {
	return &OrderBuilder{
		order: Order{
			ID:        NextOrderID(),
			Type:      OrderTypeLimit,
			TIF:       TifGTC,
			Status:    StatusNew,
			Timestamp: time.Now(),
		},
	}
}

// ID sets the order ID
func (b *OrderBuilder) ID(id uint64) *OrderBuilder {
	b.order.ID = id
	return b
}

// Symbol sets the symbol ID
func (b *OrderBuilder) Symbol(id uint64) *OrderBuilder {
	b.order.SymbolID = id
	return b
}

// Account sets the account ID
func (b *OrderBuilder) Account(id uint64) *OrderBuilder {
	b.order.AccountID = id
	return b
}

// Buy sets the order as a buy order
func (b *OrderBuilder) Buy() *OrderBuilder {
	b.order.Side = SideBuy
	return b
}

// Sell sets the order as a sell order
func (b *OrderBuilder) Sell() *OrderBuilder {
	b.order.Side = SideSell
	return b
}

// Limit sets the order as a limit order with the given price
func (b *OrderBuilder) Limit(price float64) *OrderBuilder {
	b.order.Type = OrderTypeLimit
	b.order.Price = PriceFromFloat(price)
	return b
}

// Market sets the order as a market order
func (b *OrderBuilder) Market() *OrderBuilder {
	b.order.Type = OrderTypeMarket
	return b
}

// Qty sets the order quantity
func (b *OrderBuilder) Qty(qty float64) *OrderBuilder {
	b.order.Quantity = QuantityFromFloat(qty)
	return b
}

// TimeInForce sets the time-in-force
func (b *OrderBuilder) TimeInForce(tif TimeInForce) *OrderBuilder {
	b.order.TIF = tif
	return b
}

// STPGroup sets the self-trade prevention group
func (b *OrderBuilder) STPGroup(group uint64) *OrderBuilder {
	b.order.STPGroup = group
	return b
}

// Build returns the constructed order
func (b *OrderBuilder) Build() Order {
	return b.order
}
