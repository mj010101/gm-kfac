# Order Execution Module — Design Document

**Version**: 0.3 (CEX-DEX Arbitrage Focus)
**Date**: 2026-03-15
**Author**: MinJun
**Language**: C++ (C++20)
**Target Exchanges**: Binance (CEX), Bybit (CEX), Hyperliquid (DEX)
**Project Lead**: 정우 (KFAC)

---

## 1. Overview

### 1.1 Purpose

포지션 계산 모듈(upstream)이 산출한 시그널을 입력받아, 대상 거래소에 **최소 지연(minimum latency)**으로 주문을 전송·관리하는 모듈.
백테스터가 아니며, **실시간 주문 체결 파이프라인**의 핵심 컴포넌트다.

### 1.2 System Context

전체 시스템: **Signal → Backtesting(simulated) → Execution → Post-mortem**

이 모듈은 **Execution** 레이어에 해당하며, 두 가지 경로로 시그널을 수신한다:

1. **Backtest-validated signals**: Futures/Options time series 매매 — Backtest 모듈을 거쳐 검증된 시그널. Single-leg 주문.
2. **Direct signals**: CEX-DEX 차익거래 — Backtest 없이 직행. CEX leg + DEX leg을 동시 전송하는 2-leg arb 주문.

### 1.3 Strategic Direction

차익거래 타겟: **Binance/Bybit(CEX) ↔ Hyperliquid(DEX)**

Hyperliquid은 on-chain DEX이지만 CEX급 orderbook 구조와 API를 가짐. 대형 quant 펌들이 CEX-CEX arb에 집중하고 DEX 참여를 자제하는 경향이 있어, CEX-DEX arb 영역에 경쟁이 상대적으로 적음. Hyperliquid의 ~1초 block time으로 인한 체결 확정 지연(latency asymmetry)이 진입 장벽이자 기회.

### 1.4 Design Goals

| Priority | Goal | Metric |
|----------|------|--------|
| P0 | **Latency** — 시그널 수신 → 주문 전송 지연 최소화 | < 1ms internal processing |
| P0 | **Correctness** — 충돌 주문 방지, 포지션 한도 준수 | 0 invalid orders |
| P0 | **Arb Atomicity** — CEX-DEX 2-leg의 risk window 최소화 | Track asymmetric latency |
| P1 | **Concurrency** — 다중 심볼 동시 주문 처리 | N symbols in parallel |
| P1 | **Resilience** — 거래소 장애·타임아웃 대응 | Auto-retry with backoff |
| P2 | **Observability** — 주문 상태 추적 및 로깅 | Full audit trail |

### 1.5 Scope

**In-Scope**:
- Single-leg: Crypto futures/options 매수/매도 주문 전송 (Binance, Bybit, Hyperliquid)
- 2-leg arb: CEX(Binance/Bybit) ↔ DEX(Hyperliquid) 동시 주문
- CEX-DEX latency 비대칭 처리 (CEX ~50ms fill vs DEX ~1000ms block confirmation)
- 포트폴리오 상태(cash, unrealized PnL) 기반 주문 가능 여부 검증
- 충돌 주문 감지 및 해소
- Dummy signal generator (테스트용)
- DEX-specific: block confirmation 대기, nonce 관리, tx signing latency 시뮬레이션

**Out-of-Scope**:
- 포지션 계산 로직 (upstream module 담당)
- 시장 데이터 수집 / 가격 차이 감지 (별도 signal module)
- P&L 계산 및 리포팅
- Backtesting 로직
- 실제 on-chain tx 전송 (Phase 2)

---

## 2. Architecture

### 2.1 High-Level Component Diagram

```
┌───────────────────────────────────────────────────────────────────────┐
│                      Order Execution Module                           │
│                                                                       │
│  ┌──────────────┐    ┌──────────────┐    ┌────────────────────────┐  │
│  │   Signal      │───▶│   Order      │───▶│   Exchange Gateways    │  │
│  │   Receiver    │    │   Manager    │    │                        │  │
│  │               │    │   (Router)   │    │  ┌──────────────────┐ │  │
│  └──────────────┘    └──────┬───────┘    │  │ CEX Gateway      │ │  │
│                             │            │  │ (Binance/Bybit)  │ │  │
│                      ┌──────┴───────┐    │  └──────────────────┘ │  │
│                      │              │    │  ┌──────────────────┐ │  │
│             ┌────────▼──────┐  ┌────▼──┐│  │ DEX Gateway      │ │  │
│             │ Single-Leg    │  │ Arb   ││  │ (Hyperliquid)    │ │  │
│             │ Pipeline      │  │ Coord ││  └──────────────────┘ │  │
│             └───────────────┘  └───────┘│                        │  │
│                                         └────────────────────────┘  │
│  ┌────────────────┐  ┌───────────────┐  ┌────────────────────────┐  │
│  │ Portfolio Guard │  │ Conflict      │  │ Order State Tracker    │  │
│  │                │  │ Resolver      │  │                        │  │
│  └────────────────┘  └───────────────┘  └────────────────────────┘  │
└───────────────────────────────────────────────────────────────────────┘
```

### 2.2 Component Responsibilities

#### Signal Receiver
- Upstream에서 시그널 수신
- 시그널 유효성 검증 (필수 필드, 범위 체크)
- **시그널 출처 기반 freshness threshold 분기**
  - BACKTEST_VALIDATED: stale threshold 5000ms
  - DIRECT (arb): stale threshold 200ms
- 타임스탬프 기록

#### Order Manager (Router)
- 시그널 → 주문 변환
- **라우팅 분기**: group_id 유무로 판별
  - group_id 없음 → Single-Leg Pipeline
  - group_id 있음 → Arb Coordinator
- 비동기 병렬 디스패치

#### Single-Leg Pipeline
- ConflictResolver.check(signal)
- PortfolioGuard.check_and_reserve_margin(signal)
- Exchange Gateway로 dispatch
- Fill tracking → margin 전환

#### Arb Coordinator (CEX-DEX 2-Leg 전용)
- CEX leg + DEX leg을 동시 전송
- **Latency asymmetry 관리**: CEX fill (~50ms) vs DEX block confirm (~1000ms)
- Risk window 추적 (CEX 체결 ~ DEX 확정 사이 구간)
- Leg failure → 체결된 쪽 unwind

#### Portfolio Guard
- `cash + unrealized_pnl + realized_pnl` 관리
- Margin reservation (SENT 상태 주문의 예상 마진 즉시 차감)
- Arb pair margin: 양쪽 leg의 margin을 atomic하게 예약
- 주문 전 잔고/마진 체크 → approve / reject

#### Conflict Resolver
- 심볼별 in-flight order 추적
- 동일 심볼 반대 방향 충돌 → Cancel-and-Replace / Reject
- Arb 주문은 conflict check에서 pair 단위로 처리

#### Exchange Gateways

**CEX Gateway (Binance/Bybit)**:
- REST API 주문 전송
- WebSocket fill feed
- HMAC-SHA256 signing
- 체결 확인: 즉시 (API 응답 또는 WS feed)

**DEX Gateway (Hyperliquid)**:
- REST API 주문 전송 (CEX와 유사한 인터페이스)
- EIP-712 tx signing (CEX의 HMAC보다 signing latency 높음)
- 체결 확인: **block confirmation 대기 (~1초)**
- Nonce 관리 (동시 주문 시 충돌 방지)
- Gas fee는 HL에서는 없음 (L1 자체 수수료 구조) — 단, 향후 다른 DEX 확장 시 필요

#### Order State Tracker
- 주문 생명주기 관리 (State Machine)
- CEX: SENT → ACKNOWLEDGED → FILLED (빠름)
- DEX: SENT → TX_PENDING → TX_CONFIRMED → FILLED (block confirmation 포함)
- 타임아웃 감지 및 재전송

---

## 3. Data Structures

### 3.1 Enums

```cpp
enum class Side { BUY, SELL };
enum class InstrumentType { FUTURES, OPTIONS };

enum class Exchange { BINANCE, BYBIT, HYPERLIQUID };
enum class ExchangeType { CEX, DEX };

// Exchange → ExchangeType mapping
inline ExchangeType get_exchange_type(Exchange ex) {
    switch (ex) {
        case Exchange::BINANCE:
        case Exchange::BYBIT:
            return ExchangeType::CEX;
        case Exchange::HYPERLIQUID:
            return ExchangeType::DEX;
    }
}

enum class SignalSource {
    BACKTEST_VALIDATED,  // time series signal (backtest 거친 것)
    DIRECT               // arb signal (backtest 없이 직행)
};

enum class OrderStatus {
    CREATED,
    VALIDATING,
    PENDING,
    SENT,
    TX_PENDING,      // DEX only: tx broadcast됨, block confirm 대기 중
    TX_CONFIRMED,    // DEX only: block에 포함됨
    ACKNOWLEDGED,    // CEX: 거래소 접수 확인
    PARTIAL_FILL,
    FILLED,
    CANCELLED,
    REJECTED,
    TIMEOUT,
    UNWINDING        // Arb leg failure로 인한 청산 중
};

enum class ArbPairStatus {
    ASSEMBLING,       // legs 수집 중
    DISPATCHED,       // 양쪽 전송됨
    CEX_FILLED,       // CEX leg 체결, DEX leg 대기 (risk window)
    DEX_FILLED,       // DEX leg 체결, CEX leg 대기
    ALL_FILLED,       // 양쪽 모두 체결
    UNWINDING,        // 한쪽 실패 → 다른쪽 청산 중
    FAILED,           // 전체 실패
    COMPLETED         // 정상 완료 또는 unwind 완료
};
```

### 3.2 Signal

```cpp
struct Signal {
    std::string     signal_id;        // UUID
    std::string     symbol;           // e.g., "BTC-PERP", "ETH-PERP"
    Side            side;
    InstrumentType  instrument_type;
    Exchange        exchange;
    SignalSource    signal_source;
    double          quantity;
    double          price;            // 0 = market order
    int64_t         timestamp_ns;

    // Arb pair grouping (optional)
    std::optional<std::string> group_id;   // shared by CEX leg + DEX leg
    std::optional<int>         leg_index;  // 0 = CEX leg, 1 = DEX leg

    // Options-specific (optional)
    std::optional<double>      strike_price;
    std::optional<std::string> expiry;       // e.g., "20260401"
    std::optional<std::string> option_type;  // "CALL" or "PUT"
};
```

### 3.3 Order

```cpp
struct Order {
    std::string     order_id;          // internal ID
    std::string     exchange_order_id; // exchange-assigned (post-SENT)
    Signal          signal;
    OrderStatus     status;
    double          filled_quantity = 0.0;
    double          avg_fill_price = 0.0;
    int64_t         created_at_ns = 0;
    int64_t         sent_at_ns = 0;
    int64_t         last_update_ns = 0;
    std::string     reject_reason;

    // DEX-specific
    std::optional<std::string> tx_hash;          // on-chain tx hash
    std::optional<int64_t>     tx_confirmed_ns;  // block confirmation timestamp
};
```

### 3.4 Arb Pair

```cpp
struct ArbPair {
    std::string     group_id;
    Order           cex_leg;           // Binance or Bybit
    Order           dex_leg;           // Hyperliquid
    ArbPairStatus   status = ArbPairStatus::ASSEMBLING;
    double          expected_spread_bps = 0.0;  // 기대 스프레드
    double          realized_spread_bps = 0.0;  // 실제 체결 스프레드
    int64_t         created_at_ns = 0;
    int64_t         cex_fill_ns = 0;            // CEX 체결 시각
    int64_t         dex_confirm_ns = 0;         // DEX block confirm 시각
    int64_t         risk_window_ns = 0;         // dex_confirm - cex_fill (비대칭 구간)

    int64_t         dispatch_deadline_ns = 0;   // 양쪽 leg 도착 대기 한계
};
```

### 3.5 Portfolio State

```cpp
struct PortfolioState {
    double cash = 100000.0;
    double unrealized_pnl = 0.0;
    double realized_pnl = 0.0;

    std::unordered_map<std::string, double> positions;        // symbol → net qty
    std::unordered_map<std::string, double> margin_used;      // symbol → margin locked
    std::unordered_map<std::string, double> margin_reserved;  // symbol → margin reserved (SENT, unfilled)

    double total_value() const {
        return cash + unrealized_pnl + realized_pnl;
    }

    double available_margin(double max_leverage) const {
        double total_reserved = 0.0, total_used = 0.0;
        for (auto& [_, v] : margin_reserved) total_reserved += v;
        for (auto& [_, v] : margin_used) total_used += v;
        return cash - total_reserved - total_used;
    }
};
```

---

## 4. Core Logic

### 4.1 Order Processing Pipeline

```
Signal Received
    │
    ▼
[1] Validate Signal
    │ - required fields 체크
    │ - timestamp freshness (source별 threshold)
    │     BACKTEST_VALIDATED: 5000ms
    │     DIRECT (arb):       200ms
    │ - OPTIONS: strike, expiry, option_type 필수
    │
    ▼
[2] Route
    │
    ├─ group_id 없음 (Single-Leg) ─────────────────────┐
    │                                                     │
    │   [3a] Conflict Check                               │
    │   [4a] Portfolio Guard (margin reserve)              │
    │   [5a] Dispatch via Gateway                         │
    │   [6a] Track fill → update margin                   │
    │                                                     │
    └─ group_id 있음 (Arb) ────────────────────────────┐ │
                                                        │ │
        [3b] Arb Coordinator                            │ │
             - Assemble CEX + DEX legs                  │ │
             - Atomic margin reserve (both legs)        │ │
             - Simultaneous dispatch                    │ │
             - Asymmetric fill tracking                 │ │
             - Failure → unwind                         │ │
                                                        ▼ ▼
                                                 Order State Tracker
```

### 4.2 Conflict Resolution

```cpp
enum class ConflictStrategy {
    CANCEL_AND_REPLACE,  // 기존 주문 취소 후 새 주문 전송
    REJECT_NEW,          // 새 시그널 거부
    QUEUE                // 대기
};
```

**처리 흐름 (케이스 3: 매수 미체결 중 매도 시그널):**

```
매도 시그널 수신
    │
    ▼
동일 심볼에 미체결 매수 존재?
    │
    ├─ YES ─▶ 매수 취소 요청
    │              │
    │         취소 확인 대기 (500ms timeout)
    │              │
    │         ┌────┴────┐
    │         │ 취소됨   │ 타임아웃/실패
    │         ▼         ▼
    │     매도 전송   매도 REJECT + 경고 로그
    │
    └─ NO ──▶ 정상 진행
```

### 4.3 Arb Coordinator (CEX-DEX 2-Leg)

```
Arb Signal 수신 (group_id = "arb-001", leg_index = 0)
    │
    ▼
ArbPair 존재? (group_id lookup)
    │
    ├─ NO  → 새 ArbPair 생성, leg 추가, deadline timer 시작 (100ms)
    │
    └─ YES → leg 추가
              │
              ▼
         양쪽 legs 도착?
              │
              ├─ NO  → 대기 (deadline까지)
              │         deadline 초과 → 전체 REJECT (incomplete_pair)
              │
              └─ YES → Portfolio Guard: atomic margin reserve (both legs)
                        │
                        ├─ REJECT → 전체 REJECT
                        │
                        └─ APPROVE → CEX leg + DEX leg 동시 Dispatch
                                      │
                                      ▼
                                 Fill 모니터링 (비대칭)
                                      │
                              ┌───────┴────────────────────┐
                              │                            │
                         CEX fills first             DEX fills first
                         (~50ms)                     (~1000ms, unlikely)
                              │                            │
                              ▼                            ▼
                         Status: CEX_FILLED          Status: DEX_FILLED
                         (risk window 시작)           (risk window 시작)
                              │                            │
                              ▼                            ▼
                         DEX confirms?               CEX confirms?
                              │                            │
                         ┌────┴────┐                 ┌────┴────┐
                         │ YES     │ NO/TIMEOUT      │ YES     │ NO/TIMEOUT
                         ▼         ▼                 ▼         ▼
                    ALL_FILLED  UNWINDING        ALL_FILLED  UNWINDING
                         │     (CEX를 청산)          │     (DEX를 청산)
                         ▼         │                 ▼         │
                    COMPLETED  COMPLETED/FAILED  COMPLETED  COMPLETED/FAILED
```

**Risk Window 추적:**
```
CEX leg fills at t=50ms
DEX leg confirms at t=1100ms
risk_window = 1100 - 50 = 1050ms

이 1050ms 동안 가격이 arb spread 이상으로 역행하면 손실 가능.
realized_spread_bps = (dex_fill_price - cex_fill_price) / cex_fill_price * 10000
→ 로그에 기록하여 post-mortem 분석 자료로 활용
```

### 4.4 Concurrent Order Dispatch

```cpp
class OrderDispatcher {
    std::unordered_map<Exchange, std::unique_ptr<IExchangeGateway>> gateways_;
    moodycamel::ConcurrentQueue<Signal> signal_queue_;
    std::vector<std::jthread> workers_;  // 1 per exchange

    void dispatch(const Order& order) {
        auto& gw = gateways_[order.signal.exchange];
        gw->send_order(order);  // non-blocking
    }
};
```

**원칙:**
- 심볼 간 주문: 완전 독립 (no cross-symbol blocking)
- 동일 심볼 주문: 순차 처리 (Conflict Resolver가 ordering 보장)
- Arb pair: 양쪽 leg을 거의 동시에 dispatch (CEX와 DEX worker가 병렬)

---

## 5. Exchange Gateway Design

### 5.1 Interface

```cpp
class IExchangeGateway {
public:
    virtual ~IExchangeGateway() = default;

    virtual ExchangeType exchange_type() const = 0;
    virtual Exchange exchange() const = 0;

    // 주문 전송
    virtual std::future<OrderResult> send_order(const Order& order) = 0;

    // 주문 취소
    virtual std::future<CancelResult> cancel_order(const std::string& exchange_order_id) = 0;

    // 주문 상태 조회
    virtual std::future<OrderStatusResult> query_order(const std::string& exchange_order_id) = 0;

    // Fill feed 구독
    virtual void subscribe_fills(std::function<void(const Fill&)> callback) = 0;

    // 연결 상태
    virtual bool is_connected() const = 0;
};

struct OrderResult {
    bool success;
    std::string exchange_order_id;
    OrderStatus status;          // ACKNOWLEDGED (CEX) or TX_PENDING (DEX)
    std::string error_message;
    int64_t latency_ns;          // 전송 → 응답
};

struct Fill {
    std::string exchange_order_id;
    double filled_qty;
    double fill_price;
    int64_t fill_timestamp_ns;
    bool is_final;                // true if fully filled
    std::optional<std::string> tx_hash;  // DEX only
};
```

### 5.2 CEX Gateway (Binance/Bybit)

```cpp
class ICexGateway : public IExchangeGateway {
public:
    ExchangeType exchange_type() const override { return ExchangeType::CEX; }
    // CEX는 send_order 후 즉시 ACKNOWLEDGED 또는 REJECTED
    // Fill은 WebSocket userData stream으로 수신
};
```

### 5.3 DEX Gateway (Hyperliquid)

```cpp
class IDexGateway : public IExchangeGateway {
public:
    ExchangeType exchange_type() const override { return ExchangeType::DEX; }

    // DEX-specific
    virtual int64_t estimated_block_time_ms() const = 0;  // ~1000ms for HL
    virtual uint64_t next_nonce() = 0;                     // nonce 관리
    virtual int64_t signing_latency_ns() const = 0;        // EIP-712 signing 소요 시간
};
```

### 5.4 Exchange Comparison

| Feature | Binance (CEX) | Bybit (CEX) | Hyperliquid (DEX) |
|---------|---------------|-------------|---------------------|
| **Order API** | REST `/fapi/v1/order` | REST `/v5/order/create` | REST `/exchange` |
| **Fill Feed** | WS `userData` stream | WS `execution` | WS `userFills` |
| **Rate Limit** | 2400 req/min (weighted) | 120 req/5s | 1200 req/min |
| **Auth** | HMAC-SHA256 | HMAC-SHA256 | EIP-712 signature |
| **Fill Confirm** | ~50ms (API response) | ~50ms (API response) | ~1000ms (block time) |
| **Cancel** | REST, instant | REST, instant | REST, tx-based |
| **Nonce** | N/A | N/A | Required (sequential) |
| **Options** | Binance Options | Bybit Options (USDC) | Limited (HIP-3) |

---

## 6. Test Scenarios (Dummy Data)

### Single-Leg Tests (Futures Time Series — 과제 요구사항)

#### TC1: Sequential Cross-Symbol Orders

```
Source: BACKTEST_VALIDATED
t=0ms    Signal { symbol: "BTC-PERP", side: BUY, qty: 0.1, exchange: BINANCE }
t=10ms   Signal { symbol: "ETH-PERP", side: BUY, qty: 1.0, exchange: BINANCE }

Expected:
- BTC 즉시 전송, ETH도 즉시 전송 (BTC 체결 대기 않음)
- Portfolio Guard: BTC margin 예약 후 ETH margin 체크
```

#### TC2: Concurrent Multi-Symbol Orders

```
Source: BACKTEST_VALIDATED
t=0ms    Signal { symbol: "BTC-PERP", side: BUY,  qty: 0.1, exchange: HYPERLIQUID }
t=0ms    Signal { symbol: "ETH-PERP", side: SELL, qty: 2.0, exchange: BINANCE }
t=0ms    Signal { symbol: "SOL-PERP", side: BUY,  qty: 10,  exchange: BYBIT }

Expected:
- 3개 주문 병렬 전송
- HL 주문은 DEX gateway 경유 (block confirm 대기 포함)
- 총 처리 시간 ≈ max(개별 latency), not sum
```

#### TC3: Buy Pending → Sell Signal (Conflict)

```
Source: BACKTEST_VALIDATED
t=0ms    Signal { symbol: "BTC-PERP", side: BUY, qty: 0.1, exchange: BINANCE }
         → SENT, 미체결
t=50ms   Signal { symbol: "BTC-PERP", side: SELL, qty: 0.1, exchange: BINANCE }

Expected:
- BUY 취소 요청 → 취소 확인 → SELL 전송
- 취소 실패 시 SELL REJECTED
```

#### TC4: Options Order

```
Source: BACKTEST_VALIDATED
t=0ms    Signal {
             symbol: "BTC", side: BUY, instrument_type: OPTIONS,
             qty: 1.0, strike_price: 100000, expiry: "20260501",
             option_type: "CALL", exchange: BYBIT
         }

Expected:
- Options validation (strike, expiry, type 필수)
- Bybit Options endpoint로 전송
```

#### TC5: Stale Signal Rejection

```
t=0ms    Signal { source: DIRECT, timestamp_ns: (300ms 전) }
         → DIRECT threshold 200ms 초과 → REJECTED

t=0ms    Signal { source: BACKTEST_VALIDATED, timestamp_ns: (3초 전) }
         → BT threshold 5000ms 이내 → PASS
```

#### TC6: Insufficient Margin

```
Portfolio: cash=1000
t=0ms    Signal { symbol: "BTC-PERP", side: BUY, qty: 10.0, price: 100000 }
         → Required margin (10x): 100,000 → REJECTED (insufficient_margin)
```

### CEX-DEX Arb Tests

#### TC7: CEX-DEX Arb — Both Legs Fill (Success)

```
Source: DIRECT
t=0ms    Signal { symbol: "BTC-PERP", side: BUY,  qty: 0.1, exchange: BINANCE,
                  group_id: "arb-001", leg_index: 0, price: 100000 }
t=0ms    Signal { symbol: "BTC-PERP", side: SELL, qty: 0.1, exchange: HYPERLIQUID,
                  group_id: "arb-001", leg_index: 1, price: 100050 }

Simulated:
- Binance (CEX): fill at 100000, latency 50ms
- Hyperliquid (DEX): fill at 100050, block confirm 1000ms

Expected:
- Arb Coordinator: 양쪽 동시 전송
- Status 전이: DISPATCHED → CEX_FILLED (t=50ms) → ALL_FILLED (t=1000ms)
- risk_window_ns ≈ 950ms
- realized_spread_bps ≈ 5.0 bps
- COMPLETED
```

#### TC8: CEX-DEX Arb — DEX Leg Fails (Unwind)

```
Source: DIRECT
t=0ms    Signal { symbol: "BTC-PERP", side: BUY,  qty: 0.1, exchange: BINANCE,
                  group_id: "arb-002", leg_index: 0 }
t=0ms    Signal { symbol: "BTC-PERP", side: SELL, qty: 0.1, exchange: HYPERLIQUID,
                  group_id: "arb-002", leg_index: 1 }

Simulated:
- Binance: BUY fills at 100000 (50ms)
- Hyperliquid: SELL REJECTED (e.g., insufficient liquidity on DEX)

Expected:
- Status: DISPATCHED → CEX_FILLED → UNWINDING
- Unwind: Binance에 SELL 100000 (체결된 BUY 청산)
- 청산 완료 → COMPLETED
- 로그: "arb-002: DEX leg rejected, unwinding CEX leg"
```

#### TC9: CEX-DEX Latency Asymmetry Stress Test

```
Source: DIRECT
t=0ms    Signal { symbol: "ETH-PERP", side: BUY,  qty: 1.0, exchange: BYBIT,
                  group_id: "arb-003", leg_index: 0, price: 3000 }
t=0ms    Signal { symbol: "ETH-PERP", side: SELL, qty: 1.0, exchange: HYPERLIQUID,
                  group_id: "arb-003", leg_index: 1, price: 3003 }

Simulated:
- Bybit (CEX): fill at 3000, latency 50ms
- Hyperliquid (DEX): fill at 3001 (slippage during 1200ms block time)

Expected:
- risk_window_ns ≈ 1150ms
- expected_spread_bps = 10.0 bps (3003-3000)/3000*10000
- realized_spread_bps ≈ 3.3 bps (3001-3000)/3000*10000
- Spread erosion logged: "arb-003: expected 10.0 bps, realized 3.3 bps, slippage 6.7 bps"
- Still ALL_FILLED → COMPLETED (profit reduced but positive)
```

---

## 7. Concurrency Model

### 7.1 Threading Architecture

```
Main Thread
    │
    ├── Signal Receiver Thread
    │       └── reads from signal queue
    │
    ├── Order Manager Thread (core event loop)
    │       └── route → single-leg or arb coordinator
    │
    ├── Arb Coordinator Thread
    │       └── leg assembly, pair dispatch, unwind
    │
    ├── Exchange Worker Threads
    │       ├── Binance Worker (CEX)
    │       ├── Bybit Worker (CEX)
    │       └── Hyperliquid Worker (DEX, includes block confirm simulation)
    │
    └── Fill Feed Threads
            ├── Binance Fill Feed
            ├── Bybit Fill Feed
            └── Hyperliquid Fill Feed
```

### 7.2 Synchronization

| Shared Resource | Protection |
|-----------------|------------|
| Portfolio State | `std::shared_mutex` |
| Per-Symbol Order Book | `std::mutex` per symbol |
| Signal Queue | Lock-free MPSC queue |
| Order State Map | `std::shared_mutex` |
| ArbPair Map | `std::mutex` |
| DEX Nonce Counter | `std::atomic<uint64_t>` |

---

## 8. Error Handling

| Failure | Detection | Response |
|---------|-----------|----------|
| CEX API timeout | Timer (3s) | Retry 1x → Cancel → Alert |
| DEX block confirm timeout | Timer (5s, >5x block time) | Query tx status → Retry or Cancel |
| DEX nonce conflict | Error code | Increment nonce, retry |
| WebSocket disconnect | Heartbeat miss | Auto-reconnect (exp backoff) |
| Rate limit hit | HTTP 429 | Backoff + queue |
| Arb partial fill | ArbPairStatus monitor | Unwind filled leg |

### Retry Policy

```cpp
struct RetryPolicy {
    int max_retries = 1;
    int base_delay_ms = 50;
    double backoff_multiplier = 2;
    int max_delay_ms = 500;
};
```

---

## 9. Logging & Observability

| Event | Level | Content |
|-------|-------|---------|
| Signal received | INFO | signal_id, symbol, side, qty, source |
| Order sent | INFO | order_id, exchange, exchange_type |
| CEX order filled | INFO | order_id, fill_price, latency_us |
| DEX tx broadcast | INFO | order_id, tx_hash |
| DEX tx confirmed | INFO | order_id, tx_hash, block_confirm_ms |
| Arb pair dispatched | INFO | group_id, cex_exchange, dex_exchange |
| Arb risk window | INFO | group_id, cex_fill_ms, dex_confirm_ms, window_ms |
| Arb spread realized | INFO | group_id, expected_bps, realized_bps, slippage_bps |
| Arb unwind triggered | WARN | group_id, failed_leg, unwind_action |
| Conflict detected | WARN | symbol, existing_order, new_signal |
| Order rejected | WARN | order_id, reason |
| Exchange error | ERROR | exchange, error_code, message |

### Latency Metrics

```cpp
struct LatencyMetrics {
    int64_t signal_to_validation_ns;
    int64_t validation_to_send_ns;
    int64_t send_to_ack_ns;           // CEX: send → ack
    int64_t send_to_tx_confirm_ns;    // DEX: send → block confirm
    int64_t ack_to_fill_ns;
    int64_t total_ns;
};
```

---

## 10. Build & Dependencies

### Dependencies (Phase 1 — no network)

| Library | Purpose |
|---------|---------|
| **nlohmann/json** | JSON config parsing |
| **spdlog** | Logging |
| **moodycamel::ConcurrentQueue** | Lock-free queue |
| **GoogleTest** | Testing |

### Directory Structure

```
order-execution-module/
├── CMakeLists.txt
├── config/
│   └── config.json
├── src/
│   ├── main.cpp
│   ├── core/
│   │   ├── order_manager.h/.cpp
│   │   ├── conflict_resolver.h/.cpp
│   │   ├── portfolio_guard.h/.cpp
│   │   └── arb_coordinator.h/.cpp
│   ├── gateway/
│   │   ├── i_exchange_gateway.h
│   │   ├── i_cex_gateway.h
│   │   ├── i_dex_gateway.h
│   │   ├── mock_cex_gateway.h/.cpp
│   │   └── mock_dex_gateway.h/.cpp
│   ├── model/
│   │   ├── signal.h
│   │   ├── order.h
│   │   ├── arb_pair.h
│   │   └── portfolio.h
│   ├── transport/
│   │   └── signal_receiver.h/.cpp
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
│   └── dummy_signal_generator.h/.cpp
└── README.md
```

---

## 11. Configuration (config.json)

```json
{
  "exchanges": {
    "binance": {
      "type": "CEX",
      "rest_endpoint": "https://fapi.binance.com",
      "ws_endpoint": "wss://fstream.binance.com/ws",
      "rate_limit_per_min": 2400,
      "order_timeout_ms": 3000,
      "avg_fill_latency_ms": 50
    },
    "bybit": {
      "type": "CEX",
      "rest_endpoint": "https://api.bybit.com",
      "ws_endpoint": "wss://stream.bybit.com/v5/private",
      "rate_limit_per_min": 1440,
      "order_timeout_ms": 3000,
      "avg_fill_latency_ms": 50
    },
    "hyperliquid": {
      "type": "DEX",
      "rest_endpoint": "https://api.hyperliquid.xyz",
      "ws_endpoint": "wss://api.hyperliquid.xyz/ws",
      "rate_limit_per_min": 1200,
      "order_timeout_ms": 5000,
      "avg_fill_latency_ms": 1000,
      "block_time_ms": 1000,
      "signing_latency_ms": 5
    }
  },
  "portfolio": {
    "initial_cash": 100000.0,
    "max_leverage": 10.0
  },
  "signal_freshness": {
    "BACKTEST_VALIDATED_ms": 5000,
    "DIRECT_ms": 200
  },
  "arb": {
    "assembly_deadline_ms": 100,
    "unwind_timeout_ms": 3000,
    "max_risk_window_ms": 2000
  }
}
```

---

## 12. Development Phases

### Phase 1 — Core + Mock (현재, 구현 대상)
- [ ] Data structures (Signal, Order, ArbPair, Portfolio)
- [ ] Order Manager (router: single-leg vs arb)
- [ ] Single-leg pipeline (validate → conflict → portfolio → dispatch → track)
- [ ] Portfolio Guard (margin reservation, atomic arb pair reservation)
- [ ] Conflict Resolver
- [ ] Arb Coordinator (CEX-DEX 2-leg, risk window tracking, unwind)
- [ ] Mock CEX Gateway (configurable latency/fill/reject)
- [ ] Mock DEX Gateway (block confirmation delay, nonce, signing latency)
- [ ] Dummy Signal Generator (9 test cases)
- [ ] Unit tests (9 scenarios)
- [ ] README with architecture + DEX integration roadmap

### Phase 2 — Live Exchange Integration
- [ ] Binance Futures Gateway (REST + WebSocket + HMAC)
- [ ] Bybit Futures/Options Gateway (REST + WebSocket + HMAC)
- [ ] Hyperliquid Gateway (REST + WebSocket + EIP-712 signing)
- [ ] Real nonce management for HL
- [ ] Rate limiter per exchange
- [ ] Connection pooling / reconnection

### Phase 3 — Optimization
- [ ] Lock-free data paths
- [ ] Object pooling
- [ ] CPU affinity / thread pinning
- [ ] io_uring for network I/O
- [ ] EIP-712 signing pre-computation
- [ ] Latency benchmarking

### Phase 4 — Extensions
- [ ] Additional DEX gateways (Uniswap, dYdX, etc.)
- [ ] DEX-DEX arb support
- [ ] Post-mortem module integration
- [ ] Real-time spread monitoring dashboard

---

## 13. Open Questions

| # | Question | Owner |
|---|----------|-------|
| 1 | 시그널 수신 IPC 방식 (ZMQ, shared memory, direct call) | 정우 |
| 2 | Options 대상 거래소 — Bybit만? | 정우 |
| 3 | Conflict resolution 기본 전략: CANCEL_AND_REPLACE? | 정우 |
| 4 | Market order vs Limit order — arb에서 어느 쪽 우선? | 정우 |
| 5 | Arb unwind 전략 — market order 즉시 청산? | 정우 |
| 6 | HL historical trade data 확보 방안 (signal module용) | MinJun |
| 7 | Arb spread threshold — 최소 몇 bps부터 실행? | 정우 |
| 8 | Post-mortem 입력 포맷 — JSON? Protobuf? | 정우 |
