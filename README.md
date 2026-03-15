# Order Execution Module

Real-time order dispatch pipeline for crypto quantitative trading. Handles single-leg futures/options orders and CEX-DEX (Binance/Bybit ↔ Hyperliquid) 2-leg arbitrage with latency asymmetry management. Phase 1 uses mock gateways for full pipeline simulation.

## Architecture

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

## Build Instructions

```bash
cd order-execution-module
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Requirements: CMake 3.20+, C++20 compiler (gcc 12+ / clang 15+).
Dependencies (auto-fetched): nlohmann/json, spdlog, GoogleTest.

## Running Tests

```bash
cd build
ctest --output-on-failure
# or directly:
./order_execution_tests
```

## Running Demo

```bash
cd build
./order_execution
```

Runs all 9 test scenarios (TC1-TC9) with mock gateways and prints results.

## Test Cases

| TC | Description | Type |
|----|-------------|------|
| TC1 | Sequential cross-symbol orders (BTC, ETH on Binance) | Single-leg |
| TC2 | Concurrent multi-symbol orders (3 exchanges in parallel) | Single-leg |
| TC3 | Buy pending → sell signal conflict resolution | Single-leg |
| TC4 | Options order with strike/expiry validation | Single-leg |
| TC5 | Stale signal rejection (DIRECT vs BACKTEST thresholds) | Single-leg |
| TC6 | Insufficient margin rejection | Single-leg |
| TC7 | CEX-DEX arb success — both legs fill, spread tracking | Arb |
| TC8 | CEX-DEX arb — DEX rejects, CEX leg unwind | Arb |
| TC9 | CEX-DEX arb — latency asymmetry and spread erosion | Arb |

## CEX vs DEX Execution Differences

### Why Separate Gateways

CEX and DEX have fundamentally different execution models:

- **CEX (Binance/Bybit)**: REST API order → immediate ACK → WebSocket fill feed. HMAC-SHA256 auth. Fill confirmation in ~50ms.
- **DEX (Hyperliquid)**: REST API → EIP-712 tx signing → broadcast → block confirmation (~1000ms). On-chain nonce required.

### Block Confirmation Latency & Risk Window

Hyperliquid has ~1 second block time. In a CEX-DEX arb:
1. CEX leg fills at t=50ms (position is one-sided)
2. DEX leg confirms at t=1050ms
3. **Risk window = 1000ms** — during this period, price movement can erode or eliminate the arb spread

The `ArbCoordinator` tracks `risk_window_ns` for every pair and logs it for post-mortem analysis. If the risk window exceeds `max_risk_window_ms` (default 2000ms), it triggers an unwind.

### Nonce Management

DEX orders require sequential nonces. `MockDexGateway` uses `std::atomic<uint64_t>` to ensure concurrent DEX orders don't conflict. In production, nonce management must handle gaps from failed transactions.

### EIP-712 vs HMAC Signing

- **HMAC-SHA256** (CEX): Sub-microsecond, negligible latency
- **EIP-712** (DEX): ~5ms signing latency, requires private key access. This is simulated in mock but adds real overhead in production.

## Future Extension: DEX Integration Roadmap

### Phase 2: Live Hyperliquid Integration
- EIP-712 signature implementation with ethers/secp256k1
- WebSocket connection for real-time fill feed (`userFills`)
- Real nonce management with gap recovery
- Rate limiter (1200 req/min)

### Phase 3: Additional DEX Support
- dYdX v4 (Cosmos-based, different signing)
- Uniswap v3/v4 for spot legs in cross-venue arb
- DEX-DEX arb potential (HL ↔ dYdX)

### Phase 4: MEV Protection
- Private mempool usage where available
- Flashbots-style bundle submission for EVM DEXes
- Slippage protection and sandwich attack mitigation
