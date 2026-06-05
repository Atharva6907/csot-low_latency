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

    unordered_map<string, string> symbol_pool;

    string line;
    getline(file, line); 

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
            stored,      
            bid_price,
            ask_price,
            bid_qty,
            ask_qty
        });
    }

    return ticks;
}

int main(int argc, char* argv[]) {
    auto ticks = load_ticks(argv[1]);

    unique_ptr<csot::Strategy> strategy(create_strategy());
    strategy->on_init();

    csot::LatencyHistogram hist;

    for (const auto& tick : ticks) {
        auto t1 = steady_clock::now();

        auto orders = strategy->on_tick(tick);

        auto t2 = steady_clock::now();

        auto ns = duration_cast<nanoseconds>(t2 - t1).count();
        hist.record(static_cast<uint64_t>(ns));

        for (const auto& order : orders)
            strategy->on_fill(order, order.price, order.qty);
    }

    cout << "\nLatency Summary\n";
    cout << "ticks : " << hist.count() << "\n";
    hist.print(cout);

    return 0;
}