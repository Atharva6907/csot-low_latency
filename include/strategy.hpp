// ============================================================================
//  strategy.hpp — CSoT'26 Low Latency Track, Week 1
//  THIS IS A FROZEN ABI. Do not modify.
// ============================================================================

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace csot {

struct Tick {
    uint64_t          timestamp_ns;
    std::string_view  symbol;
    double            bid_px;
    double            ask_px;
    uint32_t          bid_qty;
    uint32_t          ask_qty;
};
static_assert(sizeof(Tick) == 48, "Tick layout is part of the ABI; do not change.");

struct Order {
    enum class Side : uint8_t { BUY = 0, SELL = 1 };

    Side              side;
    std::string_view  symbol;
    double            price;
    uint32_t          qty;
};
static_assert(sizeof(Order) == 40, "Order layout is part of the ABI; do not change.");

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual void on_init() {}
    virtual std::vector<Order> on_tick(const Tick& t) = 0;
    virtual void on_fill(const Order& o, double fill_price, uint32_t fill_qty) {
        (void)o; (void)fill_price; (void)fill_qty;
    }
};

}  // namespace csot

extern "C" csot::Strategy* create_strategy();
