# Bitmap Matching Engine

A single-instrument limit order book — aggregated by price (MBP/L2) and by order (MBO/L3) —
with a price-time FIFO matching engine, implemented twice: once in safe Rust, once in
modern C++.

One design carries both. A fixed 65,536-tick price ladder sits under a three-tier
occupancy bitmap, so empty prices are never scanned. Traversal cost tracks how many levels
are **occupied**, not how far apart they sit. Neither implementation takes a third-party
dependency, and neither allocates after construction.

| | Rust | C++ |
|---|---|---|
| Source | [`rust/`](rust/) | [`cpp/`](cpp/) |
| Memory safety | `#![forbid(unsafe_code)]` | no owning raw pointers, no manual `new`/`delete` |
| Dependencies | none | none |
| Toolchain | Rust 1.97.1 (pinned) | C++23, CMake 3.24+ |
| Correctness groups | 43 | 4 suites, same properties |
| Line coverage | 97.74% | 96.80% |
| Allocation on the hot path | none | none |

## Why a bitmap ladder

A real book is sparse. Prices cluster near the touch and thin out fast, so a structure that
walks *price space* to find the next level pays for the emptiness. This one walks
*occupancy* instead.

The ladder is 65,536 ticks of flat array. Above it sit three tiers of `u64` words: one bit
per tick, then one bit per 64-tick block, then one bit per 4,096-tick block, with a single
64-bit root. Finding the best price is three `countl_zero` instructions and a load. Finding
the next occupied level away from the touch is the same. Neither depends on the distance
skipped.

That is the whole trick, and the measurements below are what it buys: walking 1,000 levels
spread across the full 65,536-tick domain costs about the same per level as walking 10
adjacent ones.

## Measurements

Both implementations were re-measured **on the same machine, back to back**, for this
repository — Windows 11 desktop, release build, no CPU pinning, minimum of three runs.

### L2 bitmap ladder

| Scenario | Rust | C++ |
|---|---:|---:|
| set level + cached BBO | 6.98 ns/update | 6.20 ns/update |
| top 10 sparse levels | 4.77 ns/level | 4.84 ns/level |
| top 1,000 sparse levels | 5.03 ns/level | 4.94 ns/level |
| VWAP across 1,000 sparse levels | 4.42 ns/level | 4.17 ns/level |

These four run the same workload in both languages and land within a few percent of each
other. That agreement is the point worth taking from this table: at the L2 layer the cost
is set by the data structure, not by the language.

### L3 book and matching engine

| Scenario | Rust | C++ |
|---|---:|---:|
| passive insert + BBO read | 11.08 ns/order | 7.08 ns/order |
| aggress 1 resting maker | 20.90 ns/fill | 18.26 ns/fill |
| aggress 64 resting makers | 11.57 ns/fill | 8.11 ns/fill |
| sweep 1,000 sparse levels | 23.60 ns/fill | 18.90 ns/fill |

**Read this table as two reports, not as a language benchmark.** The two harnesses were
written independently and scenario names that look alike do not always run the same
workload. The clearest case is amend, cancel and cancel/replace, which are omitted above
for exactly that reason: the C++ harness visits resting orders in a random permutation and
the Rust harness visits them in insertion order, so the C++ numbers carry cache misses the
Rust numbers do not. Comparing them would measure the benchmarks, not the engines.

Per-implementation tables, including the omitted rows and full percentile spreads, are in
[`rust/README.md`](rust/README.md) and [`cpp/README.md`](cpp/README.md).

All figures are batch-normalized throughput-equivalent service times over cache-resident
working sets — not tail latency, and not end-to-end exchange latency. Method in
[`docs/BENCHMARKS.md`](docs/BENCHMARKS.md). Run them yourself rather than trusting a table.

## Build and run

Each side is self-contained. Neither needs the other.

**Rust** — needs [rustup](https://rustup.rs) and nothing else:

```bash
cd rust && cargo run --release --bin bx-bench
```

The binary verifies before it measures. It runs all 43 named check groups, prints each as
`PASS` or `FAIL`, and aborts before reporting a single number if any group fails — a
benchmark from an engine that fails a correctness check is worthless.

**C++** — needs CMake 3.24+ and a C++23 compiler:

```bash
cd cpp && cmake -S . -B build && cmake --build build --config Release && ctest --test-dir build -C Release
```

Then `./build/Release/bx_bench` (or `./build/bx_bench` on a single-config generator).

## Verification

Both engines are checked the same way, and the method matters more than the count.

The randomized groups are **differential tests**. Each engine is compared against a
reference model built from deliberately different primitives — an ordered map plus plain
FIFO vectors, against the engine's bitmap ladder over a fixed slot arena. The two share no
code. Agreement is therefore evidence that both are right; it does not define what right
means. Every operation compares reject reason, fill sequence, queue order, best bid/ask,
level aggregates, a state hash, and free-list integrity, exactly.

| | Rust | C++ |
|---|---:|---:|
| Randomized bitmap ops vs an ordered set | 500,000 | 500,000 |
| Randomized L2 updates vs an ordered model | 600,000 | 600,000 |
| Randomized L3 commands vs a map + FIFO model | 1,600,000 | 1,600,000 |
| Exhaustive maker/taker/TIF combinations | 864 | 864 |
| Deterministic replay | 100,000 commands, pinned to a golden state hash | same |

Both sides build warnings-as-errors by default — `/W4 /WX` on MSVC, `-Wall -Wextra
-Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror` elsewhere, and
`-D warnings` for clippy. The C++ side also has an AddressSanitizer and
UndefinedBehaviorSanitizer configuration behind `-DBX_SANITIZERS=ON`.

Everything published in this repository was measured and verified with MSVC 19.44 and Rust
1.97.1 on Windows 11. Detail in [`docs/TESTING.md`](docs/TESTING.md).

## Layout

```text
README.md                  You are here
docs/
  ARCHITECTURE.md            Layout, complexity, invariants, design limits
  TESTING.md                 Verification contract and coverage
  BENCHMARKS.md              Measurement method

rust/
  src/lib.rs                 The engine
  src/verify/                43 check groups and the reference models
  src/bin/bench.rs           Self-verifying benchmark binary
  rust-toolchain.toml        Pins Rust 1.97.1

cpp/
  bitmap_exchange.hpp        The engine, single header, no dependencies
  bitmap_exchange.cppm       Optional C++ module interface over the same header
  bench.cpp                  Benchmark binary
  tests/                     Four suites, including the differential model
```

## Scope

This is an auditable matching **core**, not a complete exchange. It deliberately excludes
gateway decoding, account state, self-trade prevention, risk reservation, sequencing,
journaling, snapshots and recovery, market data feeds, auctions, hidden orders, pegging,
pro-rata allocation, multi-instrument sharding, networking, and failover.

Those are the parts that surround a matching engine, and they are the harder half of a
venue. [exchange-core](https://github.com/hsdxpro/exchange-core) builds them around an
engine of this shape — a binary protocol over TCP, an append-only journal as the single
source of truth, quorum-acknowledged replication, snapshots, and automatic failover.

Inside the core, the same honesty applies — several decisions that are correct for a
bounded, verifiable engine would be wrong for one taking real order flow: order IDs are
dense table indices rather than exchange or client IDs, quantities are 32-bit and prices
16-bit ticks, there is no participant identity and therefore no self-trade prevention or
fee tiers, and market orders sweep to the domain extreme with no collar. Each one, with its
cost, is written up in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Behavioral contract

A continuous, visible, single-instrument price-time FIFO book. Price priority precedes
arrival priority. Executions occur at resting-maker prices. An amend that only reduces
quantity retains queue position; any other change is a cancel/replace, which assigns a new
ID and new priority. Post-only rejects a crossing order rather than repricing it.

These follow common exchange behavior as documented by CME, Coinbase Exchange and Nasdaq.
Venue-specific auction, hidden-liquidity, routing and allocation rules are out of scope.

## License

MIT. See [LICENSE](LICENSE).
