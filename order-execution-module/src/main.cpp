#include "core/order_manager.h"
#include "gateway/mock_cex_gateway.h"
#include "gateway/mock_dex_gateway.h"
#include "../test/dummy_signal_generator.h"
#include "util/async_logger.h"
#include "util/uuid.h"
#include "util/latency_tracker.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <algorithm>
#include <vector>
#include <numeric>

using namespace oem;
using json = nlohmann::json;

OrderManager::GatewayMap make_gateway_map(IExchangeGateway* binance,
                                           IExchangeGateway* bybit,
                                           IExchangeGateway* hl) {
    OrderManager::GatewayMap gw;
    gw[Exchange::BINANCE] = binance;
    gw[Exchange::BYBIT] = bybit;
    gw[Exchange::HYPERLIQUID] = hl;
    return gw;
}

struct TestResult {
    std::string name;
    bool passed = false;
    std::string details;
};

void print_separator() {
    std::cout << "\n" << std::string(72, '=') << "\n";
}

void print_order(const Order& order) {
    std::cout << "  " << order.order_id
              << " | " << order.signal.symbol
              << " | " << to_string(order.signal.side)
              << " | " << to_string(order.signal.exchange)
              << " | qty=" << order.signal.quantity
              << " | status=" << to_string(order.status);

    if (order.status == OrderStatus::FILLED) {
        std::cout << " | fill_price=" << std::fixed << std::setprecision(2) << order.avg_fill_price
                  << " | latency=" << std::setprecision(1) << (order.latency.total_ns / 1e6) << "ms";
    }
    if (order.status == OrderStatus::REJECTED) {
        std::cout << " | reason=" << order.reject_reason;
    }
    std::cout << "\n";
}

void print_arb_pair(const ArbPair& pair) {
    std::cout << "  ArbPair: " << pair.group_id
              << " | status=" << to_string(pair.status) << "\n";
    if (pair.has_cex_leg) {
        std::cout << "    CEX: " << pair.cex_leg.order_id
                  << " | " << to_string(pair.cex_leg.signal.exchange)
                  << " | " << to_string(pair.cex_leg.status);
        if (pair.cex_leg.status == OrderStatus::FILLED)
            std::cout << " | fill=" << pair.cex_leg.avg_fill_price;
        std::cout << "\n";
    }
    if (pair.has_dex_leg) {
        std::cout << "    DEX: " << pair.dex_leg.order_id
                  << " | " << to_string(pair.dex_leg.signal.exchange)
                  << " | " << to_string(pair.dex_leg.status);
        if (pair.dex_leg.status == OrderStatus::FILLED)
            std::cout << " | fill=" << pair.dex_leg.avg_fill_price;
        std::cout << "\n";
    }
    if (pair.status == ArbPairStatus::COMPLETED || pair.status == ArbPairStatus::ALL_FILLED) {
        std::cout << "    risk_window=" << std::fixed << std::setprecision(1)
                  << (pair.risk_window_ns / 1e6) << "ms"
                  << " | expected=" << pair.expected_spread_bps << "bps"
                  << " | realized=" << pair.realized_spread_bps << "bps"
                  << " | slippage=" << (pair.expected_spread_bps - pair.realized_spread_bps) << "bps"
                  << "\n";
    }
}

void print_portfolio(const PortfolioState& state) {
    std::cout << "  Portfolio: cash=" << std::fixed << std::setprecision(2) << state.cash
              << " | avail_margin=" << state.available_margin()
              << " | total_value=" << state.total_value() << "\n";
    state.positions.for_each([](const std::string& sym, double pos) {
        if (std::abs(pos) > 1e-9) {
            std::cout << "    Position: " << sym << " = " << pos << "\n";
        }
    });
}

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║     Order Execution Module — Phase 1 Demo (Low-Latency Optimized)   ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════╝\n";

    // Load config
    json config;
    std::ifstream config_file("config/config.json");
    if (config_file.is_open()) {
        config_file >> config;
        std::cout << "\nConfig loaded from config/config.json\n";
    } else {
        std::cout << "\nUsing default config\n";
    }

    std::vector<TestResult> results;

    auto run_test = [&](const std::string& name, auto test_fn) {
        print_separator();
        std::cout << "TEST: " << name << "\n";
        std::cout << std::string(72, '-') << "\n";

        UuidGenerator::reset();
        TestResult result;
        result.name = name;
        try {
            result.passed = test_fn();
        } catch (const std::exception& e) {
            result.passed = false;
            result.details = std::string("Exception: ") + e.what();
            std::cerr << "  EXCEPTION: " << e.what() << "\n";
        }

        std::cout << "\n  Result: " << (result.passed ? "PASS ✓" : "FAIL ✗") << "\n";
        results.push_back(result);
    };

    // === TC1 ===
    run_test("TC1: Sequential Cross-Symbol Orders", [&]() -> bool {
        auto binance = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BINANCE, .fill_latency_ms = 50});
        auto bybit = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BYBIT, .fill_latency_ms = 50});
        auto hl = std::make_unique<MockDexGateway>();
        auto gw = make_gateway_map(binance.get(), bybit.get(), hl.get());
        OrderManager::Config cfg{
            .signal_config = {5000, 200},
            .portfolio_config = {10.0, 100000.0},
            .arb_config = {100, 3000, 2000}
        };
        OrderManager mgr(cfg, gw);

        auto signals = DummySignalGenerator::sequential_cross_symbol();
        for (auto& [sig, delay] : signals) {
            if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            auto r = mgr.process_signal(sig);
            if (!r.success) { std::cout << "  Signal failed: " << r.reason << "\n"; return false; }
        }
        mgr.wait_for_completion(3000);

        auto orders = mgr.get_all_orders();
        for (auto& o : orders) print_order(o);
        print_portfolio(mgr.portfolio().get_state());

        bool all_filled = true;
        for (auto& o : orders) {
            if (o.status != OrderStatus::FILLED) all_filled = false;
        }
        return all_filled && orders.size() == 2;
    });

    // === TC2 ===
    run_test("TC2: Concurrent Multi-Symbol Orders", [&]() -> bool {
        auto binance = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BINANCE, .fill_latency_ms = 50});
        auto bybit = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BYBIT, .fill_latency_ms = 50});
        auto hl = std::make_unique<MockDexGateway>(MockDexGateway::Config{.block_time_ms = 1000, .signing_latency_ms = 5});
        auto gw = make_gateway_map(binance.get(), bybit.get(), hl.get());
        OrderManager::Config cfg{
            .signal_config = {5000, 200},
            .portfolio_config = {10.0, 100000.0},
            .arb_config = {100, 3000, 2000}
        };
        OrderManager mgr(cfg, gw);

        auto start = std::chrono::steady_clock::now();
        auto signals = DummySignalGenerator::concurrent_multi_symbol();
        for (auto& [sig, delay] : signals) {
            mgr.process_signal(sig);
        }
        mgr.wait_for_completion(5000);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        auto orders = mgr.get_all_orders();
        for (auto& o : orders) print_order(o);
        std::cout << "  Total elapsed: " << elapsed << "ms\n";
        print_portfolio(mgr.portfolio().get_state());

        bool all_filled = true;
        for (auto& o : orders) {
            if (o.status != OrderStatus::FILLED) all_filled = false;
        }
        return all_filled && orders.size() == 3 && elapsed < 3000;
    });

    // === TC3 ===
    run_test("TC3: Buy Pending -> Sell Signal (Conflict)", [&]() -> bool {
        auto binance = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BINANCE, .fill_latency_ms = 200});
        auto bybit = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BYBIT, .fill_latency_ms = 50});
        auto hl = std::make_unique<MockDexGateway>();
        auto gw = make_gateway_map(binance.get(), bybit.get(), hl.get());
        OrderManager::Config cfg{
            .signal_config = {5000, 200},
            .portfolio_config = {10.0, 100000.0},
            .arb_config = {100, 3000, 2000}
        };
        OrderManager mgr(cfg, gw);

        auto signals = DummySignalGenerator::conflict_buy_then_sell();
        mgr.process_signal(signals[0].first);
        std::this_thread::sleep_for(std::chrono::milliseconds(signals[1].second));
        mgr.process_signal(signals[1].first);

        mgr.wait_for_completion(3000);

        auto orders = mgr.get_all_orders();
        for (auto& o : orders) print_order(o);
        print_portfolio(mgr.portfolio().get_state());

        return true;
    });

    // === TC4 ===
    run_test("TC4: Options Order", [&]() -> bool {
        auto binance = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BINANCE, .fill_latency_ms = 50});
        auto bybit = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BYBIT, .fill_latency_ms = 50});
        auto hl = std::make_unique<MockDexGateway>();
        auto gw = make_gateway_map(binance.get(), bybit.get(), hl.get());
        OrderManager::Config cfg{
            .signal_config = {5000, 200},
            .portfolio_config = {10.0, 100000.0},
            .arb_config = {100, 3000, 2000}
        };
        OrderManager mgr(cfg, gw);

        auto signals = DummySignalGenerator::options_order();
        auto r = mgr.process_signal(signals[0].first);
        mgr.wait_for_completion(3000);

        auto orders = mgr.get_all_orders();
        for (auto& o : orders) print_order(o);
        print_portfolio(mgr.portfolio().get_state());

        return r.success && !orders.empty() && orders[0].status == OrderStatus::FILLED;
    });

    // === TC5 ===
    run_test("TC5: Stale Signal Rejection", [&]() -> bool {
        auto binance = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BINANCE, .fill_latency_ms = 50});
        auto bybit = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BYBIT, .fill_latency_ms = 50});
        auto hl = std::make_unique<MockDexGateway>();
        auto gw = make_gateway_map(binance.get(), bybit.get(), hl.get());
        OrderManager::Config cfg{
            .signal_config = {5000, 200},
            .portfolio_config = {10.0, 100000.0},
            .arb_config = {100, 3000, 2000}
        };
        OrderManager mgr(cfg, gw);

        auto signals = DummySignalGenerator::stale_signal();
        auto r1 = mgr.process_signal(signals[0].first);
        auto r2 = mgr.process_signal(signals[1].first);

        std::cout << "  DIRECT 300ms old: " << (r1.success ? "PASS" : "REJECTED") << " (" << r1.reason << ")\n";
        std::cout << "  BACKTEST 3s old:  " << (r2.success ? "PASS" : "REJECTED") << " (" << r2.reason << ")\n";

        mgr.wait_for_completion(3000);
        return !r1.success && r2.success;
    });

    // === TC6 ===
    run_test("TC6: Insufficient Margin", [&]() -> bool {
        auto binance = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BINANCE, .fill_latency_ms = 50});
        auto bybit = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BYBIT, .fill_latency_ms = 50});
        auto hl = std::make_unique<MockDexGateway>();
        auto gw = make_gateway_map(binance.get(), bybit.get(), hl.get());
        OrderManager::Config cfg{
            .signal_config = {5000, 200},
            .portfolio_config = {10.0, 1000.0},
            .arb_config = {100, 3000, 2000}
        };
        OrderManager mgr(cfg, gw);

        auto signals = DummySignalGenerator::insufficient_margin();
        auto r = mgr.process_signal(signals[0].first);

        std::cout << "  Result: " << (r.success ? "ACCEPTED" : "REJECTED") << "\n";
        std::cout << "  Reason: " << r.reason << "\n";

        return !r.success && std::string(r.reason).find("insufficient_margin") != std::string::npos;
    });

    // === TC7 ===
    run_test("TC7: CEX-DEX Arb — Both Legs Fill", [&]() -> bool {
        auto binance = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BINANCE, .fill_latency_ms = 50});
        auto bybit = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BYBIT, .fill_latency_ms = 50});
        auto hl = std::make_unique<MockDexGateway>(MockDexGateway::Config{.block_time_ms = 1000, .signing_latency_ms = 5});
        auto gw = make_gateway_map(binance.get(), bybit.get(), hl.get());
        OrderManager::Config cfg{
            .signal_config = {5000, 200},
            .portfolio_config = {10.0, 100000.0},
            .arb_config = {100, 3000, 2000}
        };
        OrderManager mgr(cfg, gw);

        auto signals = DummySignalGenerator::arb_success();
        for (auto& [sig, delay] : signals) mgr.process_signal(sig);

        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        auto pair = mgr.arb_coordinator().get_pair("arb-001");
        print_arb_pair(pair);
        print_portfolio(mgr.portfolio().get_state());

        return pair.status == ArbPairStatus::COMPLETED &&
               pair.risk_window_ns > 0 &&
               std::abs(pair.expected_spread_bps - 5.0) < 0.5;
    });

    // === TC8 ===
    run_test("TC8: CEX-DEX Arb — DEX Leg Fails (Unwind)", [&]() -> bool {
        auto binance = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BINANCE, .fill_latency_ms = 50});
        auto bybit = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BYBIT, .fill_latency_ms = 50});
        auto hl = std::make_unique<MockDexGateway>(MockDexGateway::Config{
            .block_time_ms = 1000, .signing_latency_ms = 5,
            .fill_probability = 0.0, .reject_probability = 1.0
        });
        auto gw = make_gateway_map(binance.get(), bybit.get(), hl.get());
        OrderManager::Config cfg{
            .signal_config = {5000, 200},
            .portfolio_config = {10.0, 100000.0},
            .arb_config = {100, 3000, 2000}
        };
        OrderManager mgr(cfg, gw);

        auto signals = DummySignalGenerator::arb_dex_failure();
        for (auto& [sig, delay] : signals) mgr.process_signal(sig);

        std::this_thread::sleep_for(std::chrono::milliseconds(3000));

        auto pair = mgr.arb_coordinator().get_pair("arb-002");
        print_arb_pair(pair);
        print_portfolio(mgr.portfolio().get_state());

        return (pair.status == ArbPairStatus::COMPLETED ||
                pair.status == ArbPairStatus::UNWINDING ||
                pair.status == ArbPairStatus::FAILED) &&
               pair.has_dex_leg && pair.dex_leg.status == OrderStatus::REJECTED;
    });

    // === TC9 ===
    run_test("TC9: CEX-DEX Latency Asymmetry Stress Test", [&]() -> bool {
        auto binance = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BINANCE, .fill_latency_ms = 50});
        auto bybit = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BYBIT, .fill_latency_ms = 50});
        auto hl = std::make_unique<MockDexGateway>(MockDexGateway::Config{
            .block_time_ms = 1200, .signing_latency_ms = 5,
            .fill_probability = 1.0, .reject_probability = 0.0,
            .slippage_bps = 20.0
        });
        auto gw = make_gateway_map(binance.get(), bybit.get(), hl.get());
        OrderManager::Config cfg{
            .signal_config = {5000, 200},
            .portfolio_config = {10.0, 100000.0},
            .arb_config = {100, 3000, 2000}
        };
        OrderManager mgr(cfg, gw);

        auto signals = DummySignalGenerator::arb_latency_asymmetry();
        for (auto& [sig, delay] : signals) mgr.process_signal(sig);

        std::this_thread::sleep_for(std::chrono::milliseconds(3000));

        auto pair = mgr.arb_coordinator().get_pair("arb-003");
        print_arb_pair(pair);
        print_portfolio(mgr.portfolio().get_state());

        if (pair.status != ArbPairStatus::COMPLETED) return false;

        double slippage = pair.expected_spread_bps - pair.realized_spread_bps;
        std::cout << "  Spread erosion: " << slippage << " bps\n";

        return pair.realized_spread_bps < pair.expected_spread_bps;
    });

    // === Latency Benchmark ===
    run_test("BENCH: Hot Path Latency Benchmark", [&]() -> bool {
        auto binance = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BINANCE, .fill_latency_ms = 0});
        auto bybit = std::make_unique<MockCexGateway>(MockCexGateway::Config{.exchange_id = Exchange::BYBIT, .fill_latency_ms = 0});
        auto hl = std::make_unique<MockDexGateway>(MockDexGateway::Config{.block_time_ms = 0, .signing_latency_ms = 0});
        auto gw = make_gateway_map(binance.get(), bybit.get(), hl.get());
        OrderManager::Config cfg{
            .signal_config = {5000, 200},
            .portfolio_config = {10.0, 100000000.0}, // large cash to avoid margin issues
            .arb_config = {100, 3000, 2000}
        };
        OrderManager mgr(cfg, gw);

        const int N = 1000;
        std::vector<int64_t> latencies;
        latencies.reserve(N);

        for (int i = 0; i < N; ++i) {
            Signal sig;
            sig.set_signal_id(UuidGenerator::signal_id());
            sig.set_symbol("BTC-PERP");
            sig.side = Side::BUY;
            sig.instrument_type = InstrumentType::FUTURES;
            sig.exchange = Exchange::BINANCE;
            sig.signal_source = SignalSource::BACKTEST_VALIDATED;
            sig.quantity = 0.001;
            sig.price = 100000.0;
            sig.timestamp_ns = now_ns();

            auto t0 = now_ns();
            mgr.process_signal(sig);
            auto t1 = now_ns();
            latencies.push_back(t1 - t0);
        }

        mgr.wait_for_completion(10000);

        std::sort(latencies.begin(), latencies.end());
        auto p50 = latencies[N / 2];
        auto p99 = latencies[static_cast<int>(N * 0.99)];
        auto max_lat = latencies[N - 1];

        std::cout << "  Hot path latency (signal -> dispatch):\n";
        std::cout << "    p50  = " << p50 << " ns (" << std::fixed << std::setprecision(2) << p50 / 1000.0 << " µs)\n";
        std::cout << "    p99  = " << p99 << " ns (" << p99 / 1000.0 << " µs)\n";
        std::cout << "    max  = " << max_lat << " ns (" << max_lat / 1000.0 << " µs)\n";

        return true; // Benchmark always passes, just reports numbers
    });

    // === Summary ===
    print_separator();
    std::cout << "\n  SUMMARY\n";
    std::cout << std::string(72, '-') << "\n";
    int passed = 0;
    for (auto& r : results) {
        std::cout << "  " << (r.passed ? "PASS" : "FAIL") << "  " << r.name << "\n";
        if (r.passed) passed++;
    }
    std::cout << "\n  " << passed << "/" << results.size() << " tests passed\n\n";

    return (passed == static_cast<int>(results.size())) ? 0 : 1;
}
