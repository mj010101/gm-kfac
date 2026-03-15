# Claude Code Implementation Prompt — Order Execution Module (CEX-DEX Arb)

## Context

You are building an **Order Execution Module** for a crypto quantitative trading system. This is NOT a backtester — it is a real-time order dispatch pipeline.

The system handles two types of signals:
1. **Single-leg**: Futures/options time series orders (backtest-validated)
2. **CEX-DEX arbitrage**: Simultaneous orders on Binance/Bybit (CEX) and Hyperliquid (DEX)

The key challenge is **latency asymmetry**: CEX fills confirm in ~50ms, while Hyperliquid (DEX) requires ~1000ms block confirmation. The time between CEX fill and DEX confirmation is the "risk window" where the arb position is exposed.

Read the attached `order_execution_module_design_v03.md` for complete architecture and data structures.

---

## What to Build (Phase 1 — Core + Mock)

Build the full internal pipeline in **C++20** with mock exchange gateways. No real API calls. Everything must compile, run, and pass all 9 test scenarios.

---

## Project Structure

```
order-execution-module/
├── CMakeLists.txt
├── config/
│   └── config.json
├── src/
│   ├── main.cpp
│   ├── core/
│   │   ├── order_manager.h / .cpp
│   │   ├── conflict_resolver.h / .cpp
│   │   ├── portfolio_guard.h / .cpp
│   │   └── arb_coordinator.h / .cpp
│   ├── gateway/
│   │   ├── i_exchange_gateway.h
│   │   ├── i_cex_gateway.h
│   │   ├── i_dex_gateway.h
│   │   ├── mock_cex_gateway.h / .cpp
│   │   └── mock_dex_gateway.h / .cpp
│   ├── model/
│   │   ├── signal.h
│   │   ├── order.h
│   │   ├── arb_pair.h
│   │   └── portfolio.h
│   ├── transport/
│   │   └── signal_receiver.h / .cpp
│   └── util/
│       ├── logger.h
│       ├── latency_tracker.h
│       └── uuid.h
├── test/
│   ├── CMakeLists.txt
│   ├── test_order_manager.cpp
│   ├── test_conflict_resolver.cpp
│   ├── test_portfolio_guard.cpp
│   ├── test_arb_coordinator.cpp
│   └── dummy_signal_generator.h / .cpp
└── README.md
```

---

## Data Structures

Implement exactly as specified in the design doc. Key structures:

### model/signal.h
```cpp
enum class Side { BUY, SELL };
enum class InstrumentType { FUTURES, OPTIONS };
enum class Exchange { BINANCE, BYBIT, HYPERLIQUID };
enum class ExchangeType { CEX, DEX };
enum class SignalSource { BACKTEST_VALIDATED, DIRECT };

inline ExchangeType get_exchange_type(Exchange ex) {
    switch (ex) {
        case Exchange::BINANCE:
        case Exchange::BYBIT:     return ExchangeType::CEX;
        case Exchange::HYPERLIQUID: return ExchangeType::DEX;
    }
}

struct Signal {
    std::string     signal_id;
    std::string     symbol;
    Side            side;
    InstrumentType  instrument_type;
    Exchange        exchange;
    SignalSource    signal_source;
    double          quantity;
    double          price;            // 0 = market order
    int64_t         timestamp_ns;

    // Arb grouping (optional)
    std::optional<std::string> group_id;
    std::optional<int>         leg_index;  // 0 = CEX, 1 = DEX

    // Options (optional)
    std::optional<double>      strike_price;
    std::optional<std::string> expiry;
    std::optional<std::string> option_type;
};
```

### model/order.h
```cpp
enum class OrderStatus {
    CREATED, VALIDATING, PENDING, SENT,
    TX_PENDING,      // DEX: tx broadcast, waiting for block
    TX_CONFIRMED,    // DEX: included in block
    ACKNOWLEDGED,    // CEX: exchange accepted
    PARTIAL_FILL, FILLED, CANCELLED, REJECTED, TIMEOUT,
    UNWINDING        // arb leg failure, reversing position
};

struct Order {
    std::string     order_id;
    std::string     exchange_order_id;
    Signal          signal;
    OrderStatus     status;
    double          filled_quantity = 0.0;
    double          avg_fill_price = 0.0;
    int64_t         created_at_ns = 0;
    int64_t         sent_at_ns = 0;
    int64_t         last_update_ns = 0;
    std::string     reject_reason;

    // DEX-specific
    std::optional<std::string> tx_hash;
    std::optional<int64_t>     tx_confirmed_ns;
};
```

### model/arb_pair.h
```cpp
enum class ArbPairStatus {
    ASSEMBLING,      // collecting legs
    DISPATCHED,      // both sent
    CEX_FILLED,      // CEX leg done, DEX pending (risk window active)
    DEX_FILLED,      // DEX leg done, CEX pending (rare)
    ALL_FILLED,      // both filled
    UNWINDING,       // one failed, reversing the other
    FAILED,
    COMPLETED
};

struct ArbPair {
    std::string     group_id;
    Order           cex_leg;
    Order           dex_leg;
    ArbPairStatus   status = ArbPairStatus::ASSEMBLING;
    double          expected_spread_bps = 0.0;
    double          realized_spread_bps = 0.0;
    int64_t         created_at_ns = 0;
    int64_t         cex_fill_ns = 0;
    int64_t         dex_confirm_ns = 0;
    int64_t         risk_window_ns = 0;       // dex_confirm - cex_fill
    int64_t         dispatch_deadline_ns = 0;
};
```

### model/portfolio.h
```cpp
struct PortfolioState {
    double cash = 100000.0;
    double unrealized_pnl = 0.0;
    double realized_pnl = 0.0;
    std::unordered_map<std::string, double> positions;
    std::unordered_map<std::string, double> margin_used;
    std::unordered_map<std::string, double> margin_reserved;

    double total_value() const { return cash + unrealized_pnl + realized_pnl; }

    double available_margin(double max_leverage) const {
        double reserved = 0, used = 0;
        for (auto& [_, v] : margin_reserved) reserved += v;
        for (auto& [_, v] : margin_used) used += v;
        return cash - reserved - used;
    }
};
```

---

## Core Components

### 1. SignalReceiver (transport/signal_receiver)

Validates incoming signals:
- Required fields: signal_id, symbol, side, quantity > 0, exchange, instrument_type
- OPTIONS: require strike_price, expiry, option_type
- Freshness check:
  - BACKTEST_VALIDATED: reject if age > 5000ms
  - DIRECT: reject if age > 200ms
- Record receive timestamp

### 2. OrderManager (core/order_manager)

Central router:
```
1. Receive validated signal
2. if signal.group_id has value:
       → forward to ArbCoordinator
   else:
       → single-leg pipeline:
           a. ConflictResolver.check(signal)
           b. PortfolioGuard.reserve_margin(signal)
           c. Dispatch via appropriate gateway (CEX or DEX based on exchange type)
           d. Track order state, handle fill callbacks
3. On fill callback:
   - Update order status
   - Convert margin_reserved → margin_used
   - Update portfolio positions
```

Must handle concurrent signals without cross-symbol blocking. Use a signal queue drained by the manager thread.

### 3. ConflictResolver (core/conflict_resolver)

Detects same-symbol opposite-side conflicts:
```
- Maintain: symbol → vector<in_flight_orders> (status SENT, TX_PENDING, ACKNOWLEDGED)
- New signal for symbol with in-flight order of opposite side:
    1. Cancel existing order (500ms timeout)
    2. Cancel confirmed → proceed with new order
    3. Cancel failed/timeout → REJECT new signal
- Same side (scaling): allow
- On terminal status: remove from tracking
```

### 4. PortfolioGuard (core/portfolio_guard)

Margin management:
```
- required_margin = (qty × price) / max_leverage
  - market order (price=0): use hardcoded reference prices for mock
    {BTC: 100000, ETH: 3000, SOL: 150}
  - OPTIONS: margin = qty × price (premium)
- Single-leg: check available_margin >= required, then reserve
- Arb pair: check available_margin >= (cex_margin + dex_margin), reserve both atomically
  - If either fails → reject both, release all reservations
- On FILL: margin_reserved → margin_used, update position
- On CANCEL/REJECT: release margin_reserved
```

### 5. ArbCoordinator (core/arb_coordinator) — THIS IS THE KEY COMPONENT

CEX-DEX 2-leg arbitrage management:
```
State machine:
ASSEMBLING → DISPATCHED → CEX_FILLED / DEX_FILLED → ALL_FILLED → COMPLETED
                                                   ↘ UNWINDING → COMPLETED / FAILED

Logic:
1. Signal arrives with group_id:
   - Create or find ArbPair
   - Assign to cex_leg or dex_leg based on exchange type
   - Start deadline timer (100ms) on first leg arrival

2. Both legs present?
   NO → wait (deadline timeout → REJECT entire pair)
   YES →
     a. Calculate expected_spread_bps from leg prices
     b. PortfolioGuard.reserve_margin_pair(cex_leg, dex_leg) — atomic
     c. Dispatch BOTH legs simultaneously
     d. Status → DISPATCHED

3. Fill monitoring (ASYMMETRIC — this is the critical part):
   - CEX fills first (typical, ~50ms):
     → Status → CEX_FILLED
     → Record cex_fill_ns
     → Wait for DEX block confirmation
   - DEX confirms (~1000ms):
     → Status → ALL_FILLED
     → Record dex_confirm_ns
     → risk_window_ns = dex_confirm_ns - cex_fill_ns
     → realized_spread_bps = abs(dex_fill_price - cex_fill_price) / min_price × 10000
     → Log spread analysis
     → Status → COMPLETED

4. Failure handling:
   - DEX leg REJECTED or TIMEOUT while CEX leg FILLED:
     → Status → UNWINDING
     → Send reverse order on CEX (BUY→SELL or SELL→BUY) at market
     → Wait for unwind fill
     → Status → COMPLETED (loss logged)
   - CEX leg REJECTED while DEX leg pending:
     → Cancel DEX leg
     → Status → FAILED
   - Both fail:
     → Release margin, Status → FAILED
```

**IMPORTANT implementation detail**: The arb coordinator needs to handle the case where CEX fills almost instantly but DEX takes ~1 second. During this risk window, the position is one-sided. The coordinator MUST:
- Track the risk_window_ns for every arb pair
- Log it for post-mortem analysis
- If risk_window exceeds max_risk_window_ms (config: 2000ms), trigger unwind even if DEX is still pending

### 6. Mock Gateways

#### MockCexGateway (gateway/mock_cex_gateway)

Simulates Binance/Bybit:
```
Constructor params:
- exchange: BINANCE or BYBIT
- fill_latency_ms: default 50
- fill_probability: default 1.0
- reject_probability: default 0.0
- slippage_bps: default 0

send_order(order):
  - sleep(fill_latency_ms)  // simulate network + matching
  - random check: reject_probability → return REJECTED
  - fill_price = order.price × (1 + slippage_bps/10000) if BUY, × (1 - slippage_bps/10000) if SELL
  - return OrderResult { success=true, status=ACKNOWLEDGED }
  - Immediately schedule fill callback (simulates WS fill feed)

cancel_order(id):
  - sleep(fill_latency_ms / 2)
  - return CancelResult { success=true }
```

#### MockDexGateway (gateway/mock_dex_gateway)

Simulates Hyperliquid with block confirmation:
```
Constructor params:
- block_time_ms: default 1000
- signing_latency_ms: default 5
- fill_probability: default 1.0
- reject_probability: default 0.0
- slippage_bps: default 0

send_order(order):
  - sleep(signing_latency_ms)  // EIP-712 signing simulation
  - Generate tx_hash = "0x" + random_hex
  - return OrderResult { success=true, status=TX_PENDING, tx_hash }
  - After sleep(block_time_ms):  // block confirmation
    → random check: reject if probability hit
    → fill_price with slippage
    → Callback: TX_CONFIRMED → FILLED

cancel_order(id):
  - sleep(signing_latency_ms + block_time_ms / 2)  // cancel is also a tx
  - return CancelResult

next_nonce():
  - return atomic nonce counter++

estimated_block_time_ms():
  - return block_time_ms

signing_latency_ns():
  - return signing_latency_ms × 1000000
```

**CRITICAL: Make both gateways configurable per-test-case.** Test case 8 needs the DEX gateway to reject. Test case 9 needs higher slippage on DEX. Create helper methods or builder patterns for easy test setup.

### 7. DummySignalGenerator (test/dummy_signal_generator)

Generate signal sets for all 9 test cases. Each function returns `vector<pair<Signal, int64_t>>` where the int64_t is the delay_ms to wait before injecting the signal.

```
TC1: sequential_cross_symbol()
  → {(BTC BUY BINANCE BACKTEST, 0ms), (ETH BUY BINANCE BACKTEST, 10ms)}

TC2: concurrent_multi_symbol()
  → {(BTC BUY HYPERLIQUID BACKTEST, 0ms), (ETH SELL BINANCE BACKTEST, 0ms), (SOL BUY BYBIT BACKTEST, 0ms)}

TC3: conflict_buy_then_sell()
  → {(BTC BUY BINANCE BACKTEST, 0ms), (BTC SELL BINANCE BACKTEST, 50ms)}

TC4: options_order()
  → {(BTC CALL OPTIONS BYBIT BACKTEST, 0ms)}

TC5: stale_signal()
  → {(BTC DIRECT with timestamp 300ms ago, 0ms), (BTC BACKTEST with timestamp 3s ago, 0ms)}

TC6: insufficient_margin()
  → {(BTC BUY 10.0qty 100000price BINANCE, 0ms)}  // needs 100k margin, only 100k cash but other reservations exist

TC7: arb_success()
  → {(BTC BUY BINANCE DIRECT group=arb-001 leg=0 price=100000, 0ms),
     (BTC SELL HYPERLIQUID DIRECT group=arb-001 leg=1 price=100050, 0ms)}

TC8: arb_dex_failure()
  → {(BTC BUY BINANCE DIRECT group=arb-002 leg=0, 0ms),
     (BTC SELL HYPERLIQUID DIRECT group=arb-002 leg=1, 0ms)}
  // MockDexGateway configured with reject_probability=1.0 for this test

TC9: arb_latency_asymmetry()
  → {(ETH BUY BYBIT DIRECT group=arb-003 leg=0 price=3000, 0ms),
     (ETH SELL HYPERLIQUID DIRECT group=arb-003 leg=1 price=3003, 0ms)}
  // MockDexGateway configured with slippage_bps=20 and block_time_ms=1200
```

---

## Testing Requirements

Use GoogleTest.

### test_order_manager.cpp
- TC1: Two orders dispatched independently, both fill, margin correctly tracked
- TC2: Three orders to three different exchanges, all dispatch in parallel, verify total time ≈ max(individual)
- TC5: DIRECT stale → rejected, BACKTEST stale → passes. Verify rejection reason string
- TC6: Insufficient margin → rejected with descriptive reason

### test_conflict_resolver.cpp
- TC3: Buy in-flight, sell arrives → cancel buy → sell dispatched
- Same-side test: Two BUY signals for same symbol → no conflict, both proceed
- Cancel failure test: Mock gateway cancel returns failure → new signal rejected

### test_portfolio_guard.cpp
- Margin reservation on send, release on cancel
- margin_reserved → margin_used on fill
- Atomic arb pair reservation: both legs reserved together, one fails → both released
- TC6 scenario: exact margin math verification

### test_arb_coordinator.cpp
- TC7: Both legs fill, verify:
  - ArbPairStatus transitions: ASSEMBLING → DISPATCHED → CEX_FILLED → ALL_FILLED → COMPLETED
  - risk_window_ns ≈ 950ms (within tolerance)
  - realized_spread_bps calculated correctly
- TC8: DEX rejects, verify:
  - Status: DISPATCHED → CEX_FILLED → UNWINDING → COMPLETED
  - Unwind order sent (reverse of CEX leg)
  - Margin released after unwind
- TC9: Both fill with DEX slippage, verify:
  - expected_spread_bps vs realized_spread_bps logged
  - Slippage amount calculated correctly

---

## main.cpp

Entry point that runs all 9 scenarios:
```
1. Load config.json
2. Initialize PortfolioState
3. For each test case (TC1-TC9):
   a. Print test case header
   b. Configure mock gateways (set reject/slippage params per test)
   c. Reset portfolio to initial state
   d. Generate signals via DummySignalGenerator
   e. Inject signals with timing delays
   f. Wait for all orders to reach terminal state
   g. Print results:
      - Each order: status, fill price, latency
      - For arb pairs: risk_window, expected vs realized spread
      - Portfolio state after test
   h. Print PASS/FAIL based on expected outcomes
4. Print summary: X/9 tests passed
```

---

## Build System (CMakeLists.txt)

CMake 3.20+. Use FetchContent:
- nlohmann/json from GitHub
- spdlog from GitHub
- GoogleTest from GitHub

No Boost. Use std library: `std::thread`, `std::mutex`, `std::shared_mutex`, `std::future`, `std::condition_variable`, `std::atomic`.

Compile flags: `-std=c++20 -Wall -Wextra -O2`

Two targets:
- `order_execution` (main executable)
- `order_execution_tests` (GoogleTest executable)

---

## Key Implementation Notes

1. **Thread safety**: PortfolioState uses `std::shared_mutex`. ArbPair map uses `std::mutex`. Per-symbol conflict tracking uses per-symbol `std::mutex`.

2. **Timing simulation**: `std::this_thread::sleep_for` in mock gateways. Real async I/O in Phase 2.

3. **UUID**: Simple counter-based. `"ORD-" + zero_padded_counter` for orders, `"SIG-" + counter` for signals.

4. **Logging**: spdlog with pattern `[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v`. Named loggers: "signal_receiver", "order_manager", "conflict_resolver", "portfolio_guard", "arb_coordinator", "cex_gateway", "dex_gateway".

5. **Nanosecond timestamps**: `std::chrono::high_resolution_clock::now().time_since_epoch().count()`

6. **Config**: Load config.json at startup with nlohmann/json. Pass to components via constructor.

7. **No real network calls**: Everything is local simulation. No libcurl, OpenSSL, WebSocket needed.

8. **Rejection reasons**: Descriptive strings:
   - `"stale_signal: age 300ms exceeds DIRECT threshold 200ms"`
   - `"insufficient_margin: required 100000.0, available 1000.0"`
   - `"conflict: cancel of ORD-000001 failed, rejecting new signal"`
   - `"incomplete_pair: deadline 100ms exceeded, only 1/2 legs received"`
   - `"arb_unwind: DEX leg rejected, unwinding CEX leg ORD-000005"`

9. **Arb spread calculation**:
   ```
   If CEX is BUY and DEX is SELL:
     expected_spread_bps = (dex_price - cex_price) / cex_price × 10000
     realized_spread_bps = (dex_fill_price - cex_fill_price) / cex_fill_price × 10000
   If CEX is SELL and DEX is BUY:
     expected_spread_bps = (cex_price - dex_price) / dex_price × 10000
     realized_spread_bps = (cex_fill_price - dex_fill_price) / dex_fill_price × 10000
   slippage_bps = expected_spread_bps - realized_spread_bps
   ```

10. **DEX nonce**: `std::atomic<uint64_t>` in MockDexGateway, starting at 0, incremented on every send_order. Log nonce with each DEX order for debugging.

---

## README.md Content

Include:
1. **Project Overview**: One paragraph on what this module does
2. **Architecture Diagram**: ASCII art from design doc
3. **Build Instructions**: cmake + make commands
4. **Running Tests**: how to run GoogleTest suite
5. **Running Demo**: how to run main.cpp and what output to expect
6. **Test Cases**: Brief description of all 9 scenarios
7. **CEX vs DEX Execution Differences**: A section explaining:
   - Why CEX and DEX gateways are separated
   - Block confirmation latency and risk window concept
   - Nonce management for DEX
   - EIP-712 vs HMAC signing
8. **Future Extension: DEX Integration Roadmap**: A section outlining:
   - Phase 2: Live Hyperliquid integration (EIP-712 signing, real WebSocket)
   - Phase 3: Additional DEX support (dYdX, Uniswap for spot leg)
   - DEX-DEX arb potential
   - On-chain MEV protection considerations

---

## Acceptance Criteria

- [ ] Compiles with CMake on Linux (gcc 12+ / clang 15+)
- [ ] All 9 GoogleTest test cases pass
- [ ] main.cpp runs all scenarios, prints clear results per test
- [ ] Single-leg pipeline: TC1-TC6 all pass with correct status transitions
- [ ] Arb coordinator: TC7 shows correct risk_window and spread calculation
- [ ] Arb coordinator: TC8 triggers unwind, reverse order sent on CEX
- [ ] Arb coordinator: TC9 shows spread erosion from DEX slippage
- [ ] DEX orders show TX_PENDING → TX_CONFIRMED → FILLED transitions
- [ ] CEX orders show SENT → ACKNOWLEDGED → FILLED transitions
- [ ] Portfolio margin correctly reserved, converted, and released
- [ ] Latency metrics recorded for every order
- [ ] spdlog shows full audit trail
- [ ] README.md includes build instructions + architecture + DEX roadmap
