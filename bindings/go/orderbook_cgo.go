package luxdex

/*
#cgo CFLAGS: -I${SRCDIR}/../c
#cgo LDFLAGS: -L${SRCDIR}/../../build -lluxdex_c -lstdc++

#include "luxdex_c.h"
#include <stdlib.h>
*/
import "C"
import (
	"runtime"
	"time"
	"unsafe"
)

// CGOEngine wraps the C++ Engine via CGO
type CGOEngine struct {
	handle   C.LuxEngine
	listener TradeListener
}

// Ensure CGOEngine implements Engine
var _ Engine = (*CGOEngine)(nil)

// NewCGOEngine creates a new engine with default configuration
func NewCGOEngine() (*CGOEngine, error) {
	handle := C.lux_engine_create()
	if handle == nil {
		return nil, ErrEngineNotReady
	}

	e := &CGOEngine{handle: handle}
	runtime.SetFinalizer(e, (*CGOEngine).destroy)
	return e, nil
}

// NewCGOEngineWithConfig creates a new engine with the given configuration
func NewCGOEngineWithConfig(config EngineConfig) (*CGOEngine, error) {
	cConfig := C.LuxEngineConfig{
		worker_threads: C.size_t(config.WorkerThreads),
		max_batch_size: C.size_t(config.MaxBatchSize),
		enable_stp:     C.bool(config.EnableSelfTradePrev),
		async_mode:     C.bool(config.AsyncMode),
	}

	handle := C.lux_engine_create_with_config(&cConfig)
	if handle == nil {
		return nil, ErrEngineNotReady
	}

	e := &CGOEngine{handle: handle}
	runtime.SetFinalizer(e, (*CGOEngine).destroy)
	return e, nil
}

func (e *CGOEngine) destroy() {
	if e.handle != nil {
		C.lux_engine_destroy(e.handle)
		e.handle = nil
	}
}

// Close explicitly releases the engine resources
func (e *CGOEngine) Close() {
	e.destroy()
	runtime.SetFinalizer(e, nil)
}

func (e *CGOEngine) Start() {
	C.lux_engine_start(e.handle)
}

func (e *CGOEngine) Stop() {
	C.lux_engine_stop(e.handle)
}

func (e *CGOEngine) IsRunning() bool {
	return bool(C.lux_engine_is_running(e.handle))
}

func (e *CGOEngine) AddSymbol(symbolID uint64) bool {
	return bool(C.lux_engine_add_symbol(e.handle, C.uint64_t(symbolID)))
}

func (e *CGOEngine) RemoveSymbol(symbolID uint64) bool {
	return bool(C.lux_engine_remove_symbol(e.handle, C.uint64_t(symbolID)))
}

func (e *CGOEngine) HasSymbol(symbolID uint64) bool {
	return bool(C.lux_engine_has_symbol(e.handle, C.uint64_t(symbolID)))
}

func (e *CGOEngine) Symbols() []uint64 {
	var count C.size_t
	ptr := C.lux_engine_symbols(e.handle, &count)
	if ptr == nil || count == 0 {
		return nil
	}
	defer C.lux_symbols_free(ptr)

	// Copy to Go slice
	result := make([]uint64, count)
	symbols := (*[1 << 30]C.uint64_t)(unsafe.Pointer(ptr))[:count:count]
	for i := range result {
		result[i] = uint64(symbols[i])
	}
	return result
}

func (e *CGOEngine) PlaceOrder(order Order) OrderResult {
	cOrder := orderToC(order)
	cResult := C.lux_engine_place_order(e.handle, &cOrder)
	defer C.lux_order_result_free(&cResult)

	result := OrderResult{
		Success: bool(cResult.success),
		OrderID: uint64(cResult.order_id),
		Error:   C.GoString(&cResult.error[0]),
	}

	if cResult.trade_count > 0 && cResult.trades != nil {
		trades := (*[1 << 20]C.LuxTrade)(unsafe.Pointer(cResult.trades))[:cResult.trade_count:cResult.trade_count]
		result.Trades = make([]Trade, len(trades))
		for i, ct := range trades {
			result.Trades[i] = tradeFromC(ct)
		}
	}

	// Notify listener
	if e.listener != nil {
		for _, trade := range result.Trades {
			e.listener.OnTrade(trade)
		}
	}

	return result
}

func (e *CGOEngine) CancelOrder(symbolID, orderID uint64) CancelResult {
	cResult := C.lux_engine_cancel_order(e.handle, C.uint64_t(symbolID), C.uint64_t(orderID))

	result := CancelResult{
		Success: bool(cResult.success),
		Error:   C.GoString(&cResult.error[0]),
	}

	if cResult.has_order {
		order := orderFromC(cResult.cancelled_order)
		result.CancelledOrder = &order

		if e.listener != nil {
			e.listener.OnOrderCancelled(order)
		}
	}

	return result
}

func (e *CGOEngine) GetOrder(symbolID, orderID uint64) (*Order, bool) {
	var cOrder C.LuxOrder
	if !C.lux_engine_get_order(e.handle, C.uint64_t(symbolID), C.uint64_t(orderID), &cOrder) {
		return nil, false
	}
	order := orderFromC(cOrder)
	return &order, true
}

func (e *CGOEngine) GetDepth(symbolID uint64, levels int) MarketDepth {
	cDepth := C.lux_engine_get_depth(e.handle, C.uint64_t(symbolID), C.size_t(levels))
	defer C.lux_market_depth_free(&cDepth)

	return depthFromC(cDepth)
}

func (e *CGOEngine) BestBid(symbolID uint64) (Price, bool) {
	var price C.LuxPrice
	if !C.lux_engine_best_bid(e.handle, C.uint64_t(symbolID), &price) {
		return 0, false
	}
	return Price(price), true
}

func (e *CGOEngine) BestAsk(symbolID uint64) (Price, bool) {
	var price C.LuxPrice
	if !C.lux_engine_best_ask(e.handle, C.uint64_t(symbolID), &price) {
		return 0, false
	}
	return Price(price), true
}

func (e *CGOEngine) GetStats() EngineStats {
	cStats := C.lux_engine_get_stats(e.handle)
	return EngineStats{
		TotalOrdersPlaced:    uint64(cStats.total_orders_placed),
		TotalOrdersCancelled: uint64(cStats.total_orders_cancelled),
		TotalTrades:          uint64(cStats.total_trades),
		TotalVolume:          uint64(cStats.total_volume),
	}
}

func (e *CGOEngine) SetTradeListener(listener TradeListener) {
	e.listener = listener
}

// CGOOrderBook provides direct access to a single order book
type CGOOrderBook struct {
	handle C.LuxOrderBook
}

// GetOrderBook returns the order book for a symbol
func (e *CGOEngine) GetOrderBook(symbolID uint64) *CGOOrderBook {
	handle := C.lux_engine_get_orderbook(e.handle, C.uint64_t(symbolID))
	if handle == nil {
		return nil
	}
	return &CGOOrderBook{handle: handle}
}

func (b *CGOOrderBook) PlaceOrder(order Order) OrderResult {
	cOrder := orderToC(order)
	cResult := C.lux_orderbook_place_order(b.handle, &cOrder)
	defer C.lux_order_result_free(&cResult)

	result := OrderResult{
		Success: bool(cResult.success),
		OrderID: uint64(cResult.order_id),
		Error:   C.GoString(&cResult.error[0]),
	}

	if cResult.trade_count > 0 && cResult.trades != nil {
		trades := (*[1 << 20]C.LuxTrade)(unsafe.Pointer(cResult.trades))[:cResult.trade_count:cResult.trade_count]
		result.Trades = make([]Trade, len(trades))
		for i, ct := range trades {
			result.Trades[i] = tradeFromC(ct)
		}
	}

	return result
}

func (b *CGOOrderBook) CancelOrder(orderID uint64) CancelResult {
	cResult := C.lux_orderbook_cancel_order(b.handle, C.uint64_t(orderID))

	result := CancelResult{
		Success: bool(cResult.success),
		Error:   C.GoString(&cResult.error[0]),
	}

	if cResult.has_order {
		order := orderFromC(cResult.cancelled_order)
		result.CancelledOrder = &order
	}

	return result
}

func (b *CGOOrderBook) GetOrder(orderID uint64) (*Order, bool) {
	var cOrder C.LuxOrder
	if !C.lux_orderbook_get_order(b.handle, C.uint64_t(orderID), &cOrder) {
		return nil, false
	}
	order := orderFromC(cOrder)
	return &order, true
}

func (b *CGOOrderBook) GetDepth(levels int) MarketDepth {
	cDepth := C.lux_orderbook_get_depth(b.handle, C.size_t(levels))
	defer C.lux_market_depth_free(&cDepth)

	return depthFromC(cDepth)
}

func (b *CGOOrderBook) BidLevels() int {
	return int(C.lux_orderbook_bid_levels(b.handle))
}

func (b *CGOOrderBook) AskLevels() int {
	return int(C.lux_orderbook_ask_levels(b.handle))
}

func (b *CGOOrderBook) TotalOrders() int {
	return int(C.lux_orderbook_total_orders(b.handle))
}

// Conversion helpers

func orderToC(o Order) C.LuxOrder {
	return C.LuxOrder{
		id:           C.uint64_t(o.ID),
		symbol_id:    C.uint64_t(o.SymbolID),
		account_id:   C.uint64_t(o.AccountID),
		price:        C.LuxPrice(o.Price),
		quantity:     C.LuxQuantity(o.Quantity),
		filled:       C.LuxQuantity(o.Filled),
		side:         C.LuxSide(o.Side),
		order_type:   C.LuxOrderType(o.Type),
		tif:          C.LuxTimeInForce(o.TIF),
		status:       C.LuxOrderStatus(o.Status),
		stp_group:    C.uint64_t(o.STPGroup),
		stop_price:   C.LuxPrice(o.StopPrice),
		timestamp_ns: C.int64_t(o.Timestamp.UnixNano()),
	}
}

func orderFromC(c C.LuxOrder) Order {
	return Order{
		ID:        uint64(c.id),
		SymbolID:  uint64(c.symbol_id),
		AccountID: uint64(c.account_id),
		Price:     Price(c.price),
		Quantity:  Quantity(c.quantity),
		Filled:    Quantity(c.filled),
		Side:      Side(c.side),
		Type:      OrderType(c.order_type),
		TIF:       TimeInForce(c.tif),
		Status:    OrderStatus(c.status),
		STPGroup:  uint64(c.stp_group),
		StopPrice: Price(c.stop_price),
		Timestamp: time.Unix(0, int64(c.timestamp_ns)),
	}
}

func tradeFromC(c C.LuxTrade) Trade {
	return Trade{
		ID:              uint64(c.id),
		SymbolID:        uint64(c.symbol_id),
		BuyOrderID:      uint64(c.buy_order_id),
		SellOrderID:     uint64(c.sell_order_id),
		BuyerAccountID:  uint64(c.buyer_account_id),
		SellerAccountID: uint64(c.seller_account_id),
		Price:           Price(c.price),
		Quantity:        Quantity(c.quantity),
		AggressorSide:   Side(c.aggressor_side),
		Timestamp:       time.Unix(0, int64(c.timestamp_ns)),
	}
}

func depthFromC(c C.LuxMarketDepth) MarketDepth {
	depth := MarketDepth{
		Timestamp: time.Unix(0, int64(c.timestamp_ns)),
	}

	if c.bid_count > 0 && c.bids != nil {
		bids := (*[1 << 20]C.LuxDepthLevel)(unsafe.Pointer(c.bids))[:c.bid_count:c.bid_count]
		depth.Bids = make([]DepthLevel, len(bids))
		for i, b := range bids {
			depth.Bids[i] = DepthLevel{
				Price:      float64(b.price),
				Quantity:   float64(b.quantity),
				OrderCount: int(b.order_count),
			}
		}
	}

	if c.ask_count > 0 && c.asks != nil {
		asks := (*[1 << 20]C.LuxDepthLevel)(unsafe.Pointer(c.asks))[:c.ask_count:c.ask_count]
		depth.Asks = make([]DepthLevel, len(asks))
		for i, a := range asks {
			depth.Asks[i] = DepthLevel{
				Price:      float64(a.price),
				Quantity:   float64(a.quantity),
				OrderCount: int(a.order_count),
			}
		}
	}

	return depth
}

// GenerateOrderID generates a unique order ID using the C++ generator
func GenerateOrderID() uint64 {
	return uint64(C.lux_generate_order_id())
}

// ResetOrderIDGeneratorC resets the C++ order ID generator
func ResetOrderIDGeneratorC(start uint64) {
	C.lux_reset_order_id_generator(C.uint64_t(start))
}
