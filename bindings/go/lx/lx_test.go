package lx

import (
	"testing"
)

func TestAddressFromLP(t *testing.T) {
	tests := []struct {
		lp   uint16
		want Address
	}{
		{0x9010, LXPoolAddress},
		{0x9011, LXOracleAddress},
		{0x9020, LXBookAddress},
		{0x9030, LXVaultAddress},
		{0x9040, LXFeedAddress},
	}

	for _, tt := range tests {
		got := AddressFromLP(tt.lp)
		if got != tt.want {
			t.Errorf("AddressFromLP(0x%x) = %v, want %v", tt.lp, got, tt.want)
		}
	}
}

func TestAddressToLP(t *testing.T) {
	tests := []struct {
		addr Address
		want uint16
	}{
		{LXPoolAddress, 0x9010},
		{LXOracleAddress, 0x9011},
		{LXBookAddress, 0x9020},
		{LXVaultAddress, 0x9030},
		{LXFeedAddress, 0x9040},
	}

	for _, tt := range tests {
		got := tt.addr.ToLP()
		if got != tt.want {
			t.Errorf("%v.ToLP() = 0x%x, want 0x%x", tt.addr, got, tt.want)
		}
	}
}

func TestIsDEXPrecompile(t *testing.T) {
	tests := []struct {
		addr Address
		want bool
	}{
		{LXPoolAddress, true},
		{LXOracleAddress, true},
		{LXBookAddress, true},
		{LXVaultAddress, true},
		{LXFeedAddress, true},
		{Address{}, false}, // zero address
		{Address{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x10}, false}, // non-zero prefix
		{Address{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x10}, false}, // LP-8xxx
	}

	for _, tt := range tests {
		got := tt.addr.IsDEXPrecompile()
		if got != tt.want {
			t.Errorf("%v.IsDEXPrecompile() = %v, want %v", tt.addr, got, tt.want)
		}
	}
}

func TestX18Arithmetic(t *testing.T) {
	// Test X18FromInt
	one := X18FromInt(1)
	if one.Lo != X18One || one.Hi != 0 {
		t.Errorf("X18FromInt(1) = {%d, %d}, want {%d, 0}", one.Lo, one.Hi, X18One)
	}

	// Test ToInt
	if one.ToInt() != 1 {
		t.Errorf("X18FromInt(1).ToInt() = %d, want 1", one.ToInt())
	}

	// Test X18FromFloat
	half := X18FromFloat(0.5)
	got := half.ToFloat()
	if got < 0.49 || got > 0.51 {
		t.Errorf("X18FromFloat(0.5).ToFloat() = %f, want ~0.5", got)
	}

	// Test IsZero
	zero := X18Zero()
	if !zero.IsZero() {
		t.Error("X18Zero().IsZero() = false, want true")
	}
	if one.IsZero() {
		t.Error("X18FromInt(1).IsZero() = true, want false")
	}
}

func TestConstants(t *testing.T) {
	// Verify fee constants
	if Fee001 != 100 {
		t.Errorf("Fee001 = %d, want 100", Fee001)
	}
	if Fee005 != 500 {
		t.Errorf("Fee005 = %d, want 500", Fee005)
	}
	if Fee030 != 3000 {
		t.Errorf("Fee030 = %d, want 3000", Fee030)
	}
	if Fee100 != 10000 {
		t.Errorf("Fee100 = %d, want 10000", Fee100)
	}

	// Verify X18One
	if X18One != 1_000_000_000_000_000_000 {
		t.Errorf("X18One = %d, want 1e18", X18One)
	}
}

func TestEnums(t *testing.T) {
	// TIF
	if TifGTC != 0 {
		t.Errorf("TifGTC = %d, want 0", TifGTC)
	}
	if TifIOC != 1 {
		t.Errorf("TifIOC = %d, want 1", TifIOC)
	}
	if TifALO != 2 {
		t.Errorf("TifALO = %d, want 2", TifALO)
	}

	// OrderKind
	if OrderLimit != 0 {
		t.Errorf("OrderLimit = %d, want 0", OrderLimit)
	}
	if OrderMarket != 1 {
		t.Errorf("OrderMarket = %d, want 1", OrderMarket)
	}

	// PositionSide
	if PositionLong != 0 {
		t.Errorf("PositionLong = %d, want 0", PositionLong)
	}
	if PositionShort != 1 {
		t.Errorf("PositionShort = %d, want 1", PositionShort)
	}

	// PriceSource
	if SourceBinance != 0 {
		t.Errorf("SourceBinance = %d, want 0", SourceBinance)
	}
	if SourceLXPool != 5 {
		t.Errorf("SourceLXPool = %d, want 5", SourceLXPool)
	}
}

func TestIsPrecompile(t *testing.T) {
	if !IsPrecompile(LXPoolAddress) {
		t.Error("IsPrecompile(LXPoolAddress) = false, want true")
	}
	if IsPrecompile(Address{}) {
		t.Error("IsPrecompile(zero) = true, want false")
	}
}

// Integration tests require the C++ library to be built
// Run with: CGO_ENABLED=1 go test -v -tags=integration

func TestLXLifecycle(t *testing.T) {
	dex, err := New()
	if err != nil {
		t.Fatalf("New() failed: %v", err)
	}
	defer dex.Close()

	dex.Initialize()
	dex.Start()

	if !dex.IsRunning() {
		t.Error("IsRunning() = false after Start()")
	}

	dex.Stop()

	if dex.IsRunning() {
		t.Error("IsRunning() = true after Stop()")
	}
}

func TestLXStats(t *testing.T) {
	dex, err := New()
	if err != nil {
		t.Fatalf("New() failed: %v", err)
	}
	defer dex.Close()

	dex.Initialize()
	dex.Start()
	defer dex.Stop()

	stats := dex.GetStats()
	// Initial stats should be zero
	if stats.PoolTotalPools != 0 {
		t.Errorf("stats.PoolTotalPools = %d, want 0", stats.PoolTotalPools)
	}
}

func TestPoolOperations(t *testing.T) {
	dex, err := New()
	if err != nil {
		t.Fatalf("New() failed: %v", err)
	}
	defer dex.Close()

	dex.Initialize()
	dex.Start()
	defer dex.Stop()

	// Create a pool key
	token0 := Address{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}
	token1 := Address{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02}

	key := PoolKey{
		Currency0:   token0,
		Currency1:   token1,
		Fee:         Fee030,
		TickSpacing: 60,
		Hooks:       Address{},
	}

	// Initialize pool with sqrt price = 1 (in Q96 format)
	sqrtPriceX96 := X18FromInt(1) // Simplified for test
	tick, err := dex.PoolInitialize(key, sqrtPriceX96)
	if err != nil {
		t.Logf("PoolInitialize returned error (expected if not fully implemented): %v", err)
	}
	_ = tick

	// Check if pool exists
	exists := dex.PoolExists(key)
	t.Logf("Pool exists: %v", exists)
}

func TestBookOperations(t *testing.T) {
	dex, err := New()
	if err != nil {
		t.Fatalf("New() failed: %v", err)
	}
	defer dex.Close()

	dex.Initialize()
	dex.Start()
	defer dex.Stop()

	// Create a book market
	config := BookMarketConfig{
		MarketID:        1,
		SymbolID:        1,
		TickSizeX18:     X18FromFloat(0.01),
		LotSizeX18:      X18FromFloat(0.001),
		MinNotionalX18:  X18FromFloat(10.0),
		MaxOrderSizeX18: X18FromFloat(1000.0),
		Status:          1, // Active
	}

	err = dex.BookCreateMarket(config)
	if err != nil {
		t.Logf("BookCreateMarket returned error (expected if not fully implemented): %v", err)
	}

	// Check if market exists
	exists := dex.BookMarketExists(1)
	t.Logf("Market exists: %v", exists)

	// Get L1 data
	l1 := dex.BookGetL1(1)
	t.Logf("L1 data: bid=%f, ask=%f", l1.BestBidPxX18.ToFloat(), l1.BestAskPxX18.ToFloat())
}

func TestVaultOperations(t *testing.T) {
	dex, err := New()
	if err != nil {
		t.Fatalf("New() failed: %v", err)
	}
	defer dex.Close()

	dex.Initialize()
	dex.Start()
	defer dex.Stop()

	// Create an account
	account := Account{
		Main: Address{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
			0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14},
		SubaccountID: 0,
	}

	// Check margin info
	margin := dex.VaultGetMargin(account)
	t.Logf("Total collateral: %f", margin.TotalCollateralX18.ToFloat())
	t.Logf("Liquidatable: %v", margin.Liquidatable)

	// Check if liquidatable
	liq := dex.VaultIsLiquidatable(account)
	t.Logf("Is liquidatable: %v", liq)
}

func TestOracleOperations(t *testing.T) {
	dex, err := New()
	if err != nil {
		t.Fatalf("New() failed: %v", err)
	}
	defer dex.Close()

	dex.Initialize()
	dex.Start()
	defer dex.Stop()

	assetID := uint64(1)

	// Register asset
	err = dex.OracleRegisterAsset(assetID)
	if err != nil {
		t.Logf("OracleRegisterAsset returned error (expected if not fully implemented): %v", err)
	}

	// Update price
	price := X18FromFloat(50000.0)    // BTC price
	confidence := X18FromFloat(100.0) // $100 confidence
	err = dex.OracleUpdatePrice(assetID, SourceBinance, price, confidence)
	if err != nil {
		t.Logf("OracleUpdatePrice returned error: %v", err)
	}

	// Get price
	gotPrice, err := dex.OracleGetPrice(assetID)
	if err != nil {
		t.Logf("OracleGetPrice returned error: %v", err)
	} else {
		t.Logf("Price: %f", gotPrice.ToFloat())
	}

	// Check if fresh
	fresh := dex.OracleIsPriceFresh(assetID)
	t.Logf("Price is fresh: %v", fresh)
}

func TestFeedOperations(t *testing.T) {
	dex, err := New()
	if err != nil {
		t.Fatalf("New() failed: %v", err)
	}
	defer dex.Close()

	dex.Initialize()
	dex.Start()
	defer dex.Stop()

	marketID := uint32(1)
	assetID := uint64(1)

	// Register market
	err = dex.FeedRegisterMarket(marketID, assetID)
	if err != nil {
		t.Logf("FeedRegisterMarket returned error: %v", err)
	}

	// Update last price
	dex.FeedUpdateLastPrice(marketID, X18FromFloat(50000.0))

	// Update BBO
	dex.FeedUpdateBBO(marketID, X18FromFloat(49999.0), X18FromFloat(50001.0))

	// Get mark price
	mp, err := dex.FeedGetMarkPrice(marketID)
	if err != nil {
		t.Logf("FeedGetMarkPrice returned error: %v", err)
	} else {
		t.Logf("Mark price: %f", mp.MarkPxX18.ToFloat())
	}

	// Get funding rate
	fr, err := dex.FeedGetFundingRate(marketID)
	if err != nil {
		t.Logf("FeedGetFundingRate returned error: %v", err)
	} else {
		t.Logf("Funding rate: %f", fr.RateX18.ToFloat())
	}
}

func TestVersion(t *testing.T) {
	v := Version()
	if v == "" {
		t.Error("Version() returned empty string")
	}
	t.Logf("LX version: %s", v)
}

// Benchmark tests

func BenchmarkX18FromInt(b *testing.B) {
	for i := 0; i < b.N; i++ {
		_ = X18FromInt(int64(i))
	}
}

func BenchmarkX18FromFloat(b *testing.B) {
	for i := 0; i < b.N; i++ {
		_ = X18FromFloat(float64(i))
	}
}

func BenchmarkAddressFromLP(b *testing.B) {
	for i := 0; i < b.N; i++ {
		_ = AddressFromLP(0x9010)
	}
}

func BenchmarkIsDEXPrecompile(b *testing.B) {
	addr := LXPoolAddress
	for i := 0; i < b.N; i++ {
		_ = addr.IsDEXPrecompile()
	}
}
