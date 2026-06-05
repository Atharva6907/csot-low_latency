#include "strategy.hpp"
#include "histogram.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>

using namespace std;
using namespace chrono;

extern "C" csot::Strategy* create_strategy();

vector<csot::Tick> load_ticks(const string& path) {
    ifstream file(path);

    vector<csot::Tick> ticks;
    ticks.reserve(1'000'000);

    // Store unique symbols — map never invalidates its string values
    unordered_map<string, string> symbol_pool;

    string line;
    getline(file, line); // skip header

    while (getline(file, line)) {
        if (line.empty()) continue;

        istringstream ss(line);
        string ts, sym, bid_p, ask_p, bid_q, ask_q;

        getline(ss, ts,    ',');
        getline(ss, sym,   ',');
        getline(ss, bid_p, ',');
        getline(ss, ask_p, ',');
        getline(ss, bid_q, ',');
        getline(ss, ask_q, ',');

        // Insert symbol if not already there
        auto& stored = symbol_pool.emplace(sym, sym).first->second;

        uint64_t timestamp;
        double bid_price, ask_price;
        uint32_t bid_qty, ask_qty;

        stringstream(ts)    >> timestamp;
        stringstream(bid_p) >> bid_price;
        stringstream(ask_p) >> ask_price;
        stringstream(bid_q) >> bid_qty;
        stringstream(ask_q) >> ask_qty;

        ticks.emplace_back(csot::Tick{
            timestamp,
            stored,       // string_view into stable map storage
            bid_price,
            ask_price,
            bid_qty,
            ask_qty
        });
    }

    return ticks;
}

// Benchmark one tick and return latency in nanoseconds
static inline uint64_t run_tick(csot::Strategy& strategy,
                                 const csot::Tick& tick) {
    auto t1 = steady_clock::now();

    auto orders = strategy.on_tick(tick);

    auto t2 = steady_clock::now();

    for (const auto& order : orders)
        strategy.on_fill(order, order.price, order.qty);

    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(t2 - t1).count());
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <ticks.csv>\n";
        return 1;
    }

    auto ticks = load_ticks(argv[1]);

    unique_ptr<csot::Strategy> strategy(create_strategy());
    strategy->on_init();

    // Warmup — run 1000 ticks without recording
    // so CPU caches and branch predictors are hot
    for (int i = 0; i < 1000 && i < (int)ticks.size(); i++)
        run_tick(*strategy, ticks[i]);

    // Reset strategy for actual benchmark
    strategy.reset(create_strategy());
    strategy->on_init();

    // Record per-tick latencies
    vector<uint64_t> latencies;
    latencies.reserve(ticks.size());

    for (const auto& tick : ticks)
        latencies.push_back(run_tick(*strategy, tick));

    // Compute percentiles
    vector<uint64_t> sorted = latencies;
    sort(sorted.begin(), sorted.end());

    auto pct = [&](double q) {
        size_t idx = static_cast<size_t>(q * sorted.size());
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        return sorted[idx];
    };

    uint64_t total = 0;
    for (auto x : latencies) total += x;
    double mean_ns = static_cast<double>(total) / latencies.size();

    cout << "{\n";
    cout << "  \"ticks\": "       << latencies.size()  << ",\n";
    cout << "  \"mean_ns\": "     << mean_ns            << ",\n";
    cout << "  \"p50_ns\": "      << pct(0.50)          << ",\n";
    cout << "  \"p90_ns\": "      << pct(0.90)          << ",\n";
    cout << "  \"p99_ns\": "      << pct(0.99)          << ",\n";
    cout << "  \"p999_ns\": "     << pct(0.999)         << ",\n";
    cout << "  \"min_ns\": "      << sorted.front()     << ",\n";
    cout << "  \"max_ns\": "      << sorted.back()      << "\n";
    cout << "}\n";

    return 0;
}