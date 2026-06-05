#!/usr/bin/env python3
from __future__ import annotations
import argparse, csv, math, random, sys
from pathlib import Path

DEFAULT_SYMBOLS   = ["SYM0", "SYM1", "SYM2", "SYM3"]
DEFAULT_START_PX  = [100.00, 250.00, 50.00, 1000.00]
DEFAULT_SEED      = 42
DEFAULT_DT_NS     = 1_000_000
DEFAULT_SIGMA     = 0.0002
DEFAULT_MIN_SPREAD = 0.01
DEFAULT_BASE_QTY  = 500

def generate(rows, out_path, *, symbols=DEFAULT_SYMBOLS, start_px=DEFAULT_START_PX,
             seed=DEFAULT_SEED, dt_ns=DEFAULT_DT_NS, sigma=DEFAULT_SIGMA,
             min_spread=DEFAULT_MIN_SPREAD, base_qty=DEFAULT_BASE_QTY,
             start_ns=1_700_000_000_000_000_000):
    if len(symbols) != len(start_px):
        sys.exit("symbols and start_px must have the same length")
    rng, n_sym, mids, ts = random.Random(seed), len(symbols), list(start_px), start_ns
    with out_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["timestamp_ns","symbol","bid_px","ask_px","bid_qty","ask_qty"])
        for i in range(rows):
            idx = i % n_sym
            mids[idx] *= math.exp(rng.gauss(0.0, sigma))
            half_spread = max(min_spread / 2.0, mids[idx] * 0.00005)
            bid = round(mids[idx] - half_spread, 4)
            ask = round(mids[idx] + half_spread, 4)
            bid_qty = max(1, int(base_qty * math.exp(rng.gauss(0.0, 0.3))))
            ask_qty = max(1, int(base_qty * math.exp(rng.gauss(0.0, 0.3))))
            w.writerow([ts, symbols[idx], f"{bid:.4f}", f"{ask:.4f}", bid_qty, ask_qty])
            ts += dt_ns + rng.randint(1, max(1, dt_ns // 10))

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rows", type=int, default=10_000)
    p.add_argument("--out",  type=Path, default=Path("synthetic_small.csv"))
    p.add_argument("--seed", type=int, default=DEFAULT_SEED)
    args = p.parse_args()
    generate(args.rows, args.out, seed=args.seed)
    print(f"wrote {args.rows} ticks to {args.out} (seed={args.seed})")

if __name__ == "__main__":
    main()
