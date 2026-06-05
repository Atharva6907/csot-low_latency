# Quant Platform — Low Latency Market Replay Engine

A from-scratch C++20 market data replay engine implementing a deterministic mean-reversion 
strategy, built for the CSoT'26 Low Latency Track (Week 1). The goal is not to invent a 
trading signal — it is to implement a fixed algorithm as fast as possible, measured in 
nanoseconds per tick.

---

## How to Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Optional — build with sanitizers for debugging:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-asan -j$(nproc)
```

---

## How to Run

Generate data:

```bash
python3 data/gen.py --rows 10000    --out data/synthetic_small.csv
python3 data/gen.py --rows 10000000 --out data/synthetic_large.csv
```

Run the replay engine:

```bash
./build/quant_runner data/tiny.csv
./build/quant_runner data/synthetic_small.csv
./build/quant_runner data/synthetic_large.csv
```

Run the benchmark:

```bash
./build/quant_bench data/synthetic_small.csv
```

---

## Hardware

| Property | Value |
|---|---|
| Machine | MacBook Air M5 |
| CPU | Apple M5 (ARM Cortex-X, 4 performance + 6 efficiency cores) |
| RAM | 16 GB LPDDR5 |
| Host OS | macOS Sequoia 15 |
| VM Software | UTM 4.7 (Apple Hypervisor) |
| VM OS | Ubuntu 24.04 LTS |
| VM Kernel | Linux 6.8 (ARM64) |
| Compiler | GCC 15.2.0 |
| Build Flags | -O2 -march=native -flto |

---

## Headline Latency Numbers

Measured locally on the VM against `synthetic_large.csv` (10,000,000 ticks):

| Metric | Local (VM) |
|---|---|
| p50 | 256 ns |
| p90 | 512 ns |
| p99 | 512 ns |
| p999 | 512 ns |

Measured by the live judge on x86 EC2 (held-out 10M tick dataset):

| Metric | Judge (EC2 x86) |
|---|---|
| p50 | 44 ns |
| p99 | ~80 ns |

Order stream correctness verified against the reference implementation on 
`tiny.csv` (20 ticks) and `synthetic_small.csv` (10,000 ticks). Every emitted 
order matches the reference on tick index, symbol, side, price, and quantity.

---

## How the Judge Works

Your compiled `spec_strategy.so` is loaded by the judge via `dlopen`. It is 
replayed against a held-out 10,000,000 tick dataset that you never see. The 
judge calls `on_tick(...)` once per tick in timestamp order, collects every 
order you emit, and compares your full order stream against its own reference 
implementation.

Correctness is binary. If a single emitted order disagrees with the reference 
on any field — tick index, symbol, side, price, or quantity — your submission 
is marked incorrect and does not rank. There are no partial marks.

Only among correct submissions does performance matter. Every `on_tick(...)` 
call is timed with a nanosecond-precision clock. The leaderboard ranks by p50, 
then p99, then p999. Trading performance — whether the strategy makes money — 
is not part of the score.

---

## File by File Breakdown

### `include/strategy.hpp` — Frozen ABI

This file defines the three core types that the entire platform is built around.
It must never be modified — the judge loads your `.so` and expects these exact
memory layouts.

**`Tick`** — one snapshot of the market at one instant:
```cpp
struct Tick {
    uint64_t         timestamp_ns;  // nanoseconds since epoch
    std::string_view symbol;        // e.g. "SYM0"
    double           bid_px;        // best bid price
    double           ask_px;        // best ask price
    uint32_t         bid_qty;       // quantity at best bid
    uint32_t         ask_qty;       // quantity at best ask
};
```

`string_view` is used instead of `string` — it is a lightweight pointer + length 
into existing memory. Zero copy, zero allocation. The engine owns the underlying 
string storage.

**`Order`** — a trading decision returned by the strategy:
```cpp
struct Order {
    Side             side;    // BUY or SELL
    std::string_view symbol;
    double           price;   // limit price
    uint32_t         qty;     // quantity
};
```

**`Strategy`** — the abstract base class with three virtual methods:
- `on_init()` — called once before the first tick. Allocate everything here.
  After this returns, the hot path begins and no heap allocation should occur.
- `on_tick(tick)` — called once per tick. This is the measured hot path.
  Returns a vector of orders.
- `on_fill(order, price, qty)` — called when an order fills. Used to update
  position state.

The `static_assert` lines are compile-time checks — if anyone accidentally
changes the struct layout, the build fails immediately instead of producing
mysterious runtime crashes.

---

### `include/histogram.hpp` — Latency Histogram

A fixed-bucket exponential histogram for recording nanosecond latencies without
any heap allocation. Uses 32 buckets where bucket `i` covers the range
`[2^i ns, 2^(i+1) ns)`. So bucket 0 covers 1-2ns, bucket 7 covers 128-256ns,
bucket 9 covers 512-1024ns, and so on up to ~4 seconds.

**`record(ns)`** — called after every `on_tick`. Takes ~3 CPU instructions:
one `__builtin_clzll` (count leading zeros) to find the right bucket, one
array increment. No branches, no allocations.

**`percentile(q)`** — walks the buckets accumulating counts until it reaches
the q-th percentile. Returns the upper bound of that bucket. Coarse but fast.

**`print(os)`** — prints p50, p90, p99, p999 to any output stream.

---

### `strategies/spec_strategy.cpp` — Mean Reversion Strategy

Implements the fixed algorithm from `STRATEGY_SPEC.md`. The trading idea is
simple: prices tend to revert to their recent average. If the current price
is unusually far from its recent mean, bet on it coming back.

#### Per-Symbol State

```cpp
struct alignas(64) SymbolState {
    double        mids[64]{};   // ring buffer of last 64 mid prices
    double        sum    = 0.0; // rolling sum for O(1) mean
    uint32_t      count  = 0;   // prices seen so far, capped at 64
    uint32_t      head   = 0;   // next write position in ring buffer
    int32_t       position = 0; // current holding: -1, 0, or +1
};
```

`alignas(64)` places each symbol's state on its own 64-byte CPU cache line.
Without this, two symbols could share a cache line causing false sharing —
one symbol's update invalidating another's cache entry unnecessarily.

#### Symbol Lookup

```cpp
static inline int sym_index(std::string_view s) {
    int h = 0;
    for (char c : s) h = h * 31 + c;
    return ((h % 64) + 64) % 64;
}
```

Maps any symbol string to an index 0-63 using a polynomial hash. Replaces
the original `unordered_map` which was doing a heap allocation and hash
lookup on every single tick. A plain array index lookup is ~1ns vs ~50ns
for a hash map lookup.

#### Ring Buffer Update

```cpp
const double old  = st.mids[st.head];
st.sum           += mid - old;
st.mids[st.head]  = mid;
st.head           = (st.head + 1) & 63;
```

The ring buffer overwrites the oldest price with the newest. `& 63` wraps
the head pointer back to 0 after 63 — equivalent to `% 64` but one CPU
instruction instead of a division. The rolling sum is updated in O(1) by
subtracting the evicted value and adding the new one.

#### Mean and Standard Deviation

```cpp
const double mean   = st.sum / 64.0;

double sq = 0.0;
for (double x : st.mids) {
    const double d = x - mean;
    sq += d * d;
}
const double stddev = std::sqrt(sq / 64.0);
```

Mean is O(1) from the rolling sum. Variance still requires a loop over all
64 prices — this is the bottleneck but it cannot be changed without risking
floating point divergence from the reference implementation. Population
standard deviation is used (denominator 64, not 63).

#### Z-score and Signal

```cpp
const double z     = (mid - mean) / stddev;
const double abs_z = std::fabs(z);
```

The z-score measures how many standard deviations the current price is from
the mean. Entry and exit rules:

| Condition | Action |
|---|---|
| `position == 0` and `z >= 2.0` | SELL 1 unit at bid |
| `position == 0` and `z <= -2.0` | BUY 1 unit at ask |
| `position > 0` and `abs_z <= 0.5` | SELL to flatten |
| `position < 0` and `abs_z <= 0.5` | BUY to flatten |

#### Allocation-Free Order Return

```cpp
static thread_local std::vector<csot::Order> orders;
orders.clear();
```

Instead of constructing a new `std::vector` on every tick (which calls
`malloc`), a `thread_local static` vector is reused. `clear()` sets the
size to zero without freeing memory. Zero heap allocation on the hot path
after warmup.

#### Branch Prediction Hints

```cpp
if (__builtin_expect(z >= ENTRY_Z, 0))
```

`__builtin_expect(condition, expected)` tells the CPU that this branch is
almost never taken (0 = false). The CPU arranges its instruction pipeline
to favor the common path (no order) reducing branch misprediction penalties.

---

### `src/engine.cpp` — Replay Engine

The engine has three responsibilities: load ticks from disk, drive the
strategy hot loop, and record latency.

#### CSV Loader

```cpp
vector<csot::Tick> load_ticks(const string& path)
```

Opens the CSV, skips the header row, and parses each line into a `Tick`.
Each line has six comma-separated fields: `timestamp_ns`, `symbol`,
`bid_px`, `ask_px`, `bid_qty`, `ask_qty`.

Symbol strings are stored in a persistent `unordered_map` so that
`Tick::symbol` (a `string_view`) always points to stable memory. If the
underlying string were to move — for example, from a vector reallocation —
all `string_view` pointers would become dangling, causing a segfault.

#### Hot Loop

```cpp
for (const auto& tick : ticks) {
    auto t1 = steady_clock::now();
    auto orders = strategy->on_tick(tick);
    auto t2 = steady_clock::now();

    auto ns = duration_cast<nanoseconds>(t2 - t1).count();
    hist.record(static_cast<uint64_t>(ns));

    for (const auto& order : orders)
        strategy->on_fill(order, order.price, order.qty);
}
```

`steady_clock` is a monotonic clock — it never goes backward unlike
`system_clock` which can jump due to NTP sync or daylight savings. Only
`on_tick` is timed — the fill simulation and histogram recording happen
outside the measurement window.

The fill model is deterministic: every valid order fills immediately at its
submitted price. This matches the judge's fill model exactly.

---

### `bench/bench.cpp` — Benchmark Driver

A standalone benchmark that measures per-tick latency and outputs JSON for
easy plotting or comparison.

#### Warmup Phase

```cpp
for (int i = 0; i < 1000 && i < (int)ticks.size(); i++)
    run_tick(*strategy, ticks[i]);
```

Runs 1000 ticks without recording. This warms up the CPU instruction cache,
branch predictor, and data caches so the measured numbers reflect steady-state
performance rather than cold-start behavior.

#### Measurement Phase

Runs the full dataset recording each tick's latency into a `vector<uint64_t>`.
After all ticks are processed, sorts the vector and computes exact percentiles
by index rather than using the exponential buckets of the histogram.

#### JSON Output

```json
{
  "ticks": 10000,
  "mean_ns": 287.4,
  "p50_ns": 256,
  "p90_ns": 384,
  "p99_ns": 512,
  "p999_ns": 1024,
  "min_ns": 128,
  "max_ns": 4096
}
```

JSON output makes it easy to pipe into Python for plotting, store in a
results file for comparison across runs, or feed into a dashboard.

---

### `data/gen.py` — Synthetic Tick Generator

Generates realistic market data using Geometric Brownian Motion — the
standard mathematical model for stock price evolution. Each symbol's
mid-price evolves as:

mid(t+1) = mid(t) * exp(gauss(0, sigma))

With `sigma = 0.0002` per tick. The generator is seeded (`seed=42`) so
every run produces the same output — important for fair benchmark
comparisons across different machines and implementations.

The spread (`ask - bid`) is always at least `0.01`. Quantities follow a
log-normal distribution around a base of 500 units.

---

## Optimizations Applied

Starting from the reference implementation at ~200ns p50 on the judge,
the following optimizations were applied in order:

### Round 1 — 202ns → 109ns

**Fixed array instead of `unordered_map`**

The reference uses:
```cpp
std::unordered_map<std::string, SymbolState> state_;
auto& st = state_[std::string(t.symbol)];
```

This does three expensive things per tick: constructs a temporary `std::string`
(heap allocation), computes a hash, and does a hash table lookup with possible
cache misses. Replaced with:

```cpp
SymbolState state_[64];
SymbolState& st = state_[sym_index(t.symbol)];
```

One array index operation. ~50ns saved per tick.

**Rolling sum for mean**

The reference recomputes the mean by summing all 64 prices every tick — O(64)
additions. Replaced with an incremental update:

```cpp
st.sum += mid - old;  // subtract evicted, add new — O(1)
const double mean = st.sum / 64.0;
```

**Cache line alignment**

`alignas(64)` ensures each `SymbolState` occupies exactly one cache line.
Without alignment, a struct could span two lines, requiring two memory fetches.

### Round 2 — 109ns → 44ns

**`thread_local` order vector**

The return type `std::vector<csot::Order>` normally allocates heap memory
on every tick that returns an order. A `thread_local static` vector is
allocated once and reused — `clear()` just sets size to zero.

**`__builtin_expect` hints**

Most ticks return no order — either in warmup or when the z-score is between
the entry and exit thresholds. `__builtin_expect` tells the compiler and CPU
that the no-order path is the common case, improving branch prediction.

---

## What Surprised Me

The thing that surprised me most was how fragile floating point arithmetic is
at boundary conditions. When I tried to eliminate the `sqrt` call by comparing
`z²` against squared thresholds (mathematically equivalent), the judge rejected
the submission because a single tick's order disagreed with the reference.

The z-score at tick 253 was right at the entry boundary of 2.0. Computing
`z >= 2.0` after a `sqrt` and computing `z² >= 4.0` without it gave different
results in the last few bits of the double — enough to flip the comparison.
This meant our submission emitted an order at tick 257 instead of 253.

This taught me that in low-latency systems, correctness and performance are
often in direct tension. You cannot blindly apply mathematical identities to
floating point code — the order of operations matters at the bit level.

---

## Real World Applications

This kind of infrastructure is the foundation of every systematic trading
operation. In practice:

**Market Making** — firms like Citadel Securities and Jane Street run engines
exactly like this one, processing millions of ticks per second and making
buy/sell decisions in under 10 microseconds. The difference between 44ns and
21ns at scale translates directly to being first or last in the queue.

**Statistical Arbitrage** — the mean-reversion signal implemented here is a
simplified version of pairs trading strategies used by quantitative hedge funds.
Real implementations track correlations across hundreds of symbols simultaneously.

**Risk Management** — real-time position tracking and risk limits require the
same low-latency infrastructure. Every fill must update position state before
the next tick arrives.

**Backtesting** — the replay engine built here is exactly what backtesting
systems use. Instead of measuring latency, production backtesting systems
simulate a full order book and measure strategy PnL.

The engineering principles here — zero allocation on the hot path, cache
locality, branch prediction, nanosecond timing — are directly transferable
to any latency-sensitive system: high-frequency trading, real-time bidding
in ad-tech, low-latency networking, and game engines.
