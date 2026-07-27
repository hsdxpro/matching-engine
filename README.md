<h1 align="center">Matching Engine</h1>

<p align="center">
  A limit order book and price-time matching engine on a bitmap price ladder.<br>
  Implemented twice, in safe Rust and in C++20, and verified against independent reference models.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/rust-1.97.1-000000?logo=rust" alt="Rust 1.97.1">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus" alt="C++20">
  <img src="https://img.shields.io/badge/dependencies-0-success" alt="Zero dependencies">
  <img src="https://img.shields.io/badge/unsafe-forbidden-success" alt="Forbid unsafe">
  <img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT">
</p>

<p align="center">
  <a href="#quick-start">Quick start</a> ·
  <a href="#benchmarks">Benchmarks</a> ·
  <a href="#how-it-works">How it works</a> ·
  <a href="#verification">Verification</a> ·
  <a href="docs/ARCHITECTURE.md">Architecture</a> ·
  <a href="https://github.com/hsdxpro/exchange-core">The venue built on it</a>
</p>

---

- **4.9 ns** to walk a price level, whether the book is dense or spread across 65,536 ticks
- **8.1 ns** per fill when sweeping a deep queue
- **Zero dependencies** and zero allocation after construction, in both languages
- **2.7M randomized operations** checked against models that share no code with the engine

## Quick start

Each implementation is self-contained. Neither needs the other.

<table>
<tr><th>Rust</th><th>C++</th></tr>
<tr valign="top"><td>

```bash
cd rust
cargo run --release --bin bx-bench
```

Verifies, then measures. Needs only
[rustup](https://rustup.rs) — the toolchain is
pinned and there is nothing to download.

</td><td>

```bash
cd cpp
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

Then `./build/Release/bx_bench`.
Needs CMake 3.24+ and a C++20 compiler.

</td></tr>
</table>

The Rust binary runs all 43 correctness groups before measuring, and aborts without
printing a number if any fail.

## How it works

A real book is sparse: prices cluster at the touch and thin out fast. Walking *price
space* pays for the emptiness between levels, so this walks *occupancy* instead.

65,536 ticks of flat array under three tiers of `u64` summary words — one bit per tick,
one per 64-tick block, one per 4,096-tick block, one 64-bit root.

```
root       1 word    ────────────────  64 bits
tier 2    64 words   ──── ──── ────     4,096 bits
tier 1  1,024 words  ─ ─ ─ ─ ─ ─ ─ ─    65,536 bits
ladder  65,536 price levels
```

Best price is three `countl_zero` instructions and a load. The next occupied level costs
the same. **Neither depends on how far the scan skips** — that is what the benchmarks
below measure.

## Benchmarks

One machine, back to back: Windows 11, release builds, no pinning, three runs.
Batch-normalized service times over cache-resident data — not tail latency, not
end-to-end exchange latency.

#### L2 aggregated book

| Scenario | Rust | C++ |
|---|---:|---:|
| Set level + cached BBO | 6.98 ns | **6.20 ns** |
| Walk top 10 sparse levels | 4.77 ns/level | 4.84 ns/level |
| Walk top 1,000 sparse levels | 5.03 ns/level | **4.94 ns/level** |
| VWAP across 1,000 sparse levels | 4.42 ns/level | **4.17 ns/level** |

1,000 levels spread across the full 65,536-tick domain: **4.9 ns each**, against 4.8 ns
for 10 adjacent. Spreading the book out is close to free — the property the bitmap
exists to provide. Both languages land within a few percent, so the structure sets the
cost, not the compiler.

#### L3 order-by-order book and matching

| Scenario | Rust | C++ |
|---|---:|---:|
| Passive insert + BBO read | 11.08 ns | **7.08 ns** |
| Match 1 resting maker | 20.90 ns/fill | **18.26 ns/fill** |
| Match 64 resting makers | 11.57 ns/fill | **8.11 ns/fill** |
| Sweep 1,000 sparse levels | 23.60 ns/fill | **18.90 ns/fill** |

> **Two reports, not a language benchmark.** The harnesses were written independently.
> Amend, cancel and replace are absent for that reason: the C++ harness visits resting
> orders in a random permutation, the Rust one in insertion order, so C++ absorbs cache
> misses Rust never sees. Per-language tables with percentiles in
> [`rust/README.md`](rust/README.md) and [`cpp/README.md`](cpp/README.md); method in
> [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md).

## Verification

**Differential tests.** Each engine runs against a reference model built from different
primitives — an ordered map plus FIFO vectors — sharing no code with the engine.
Agreement is evidence both are correct; it cannot define what correct means.

| Check | Volume |
|---|---:|
| Randomized bitmap ops vs an ordered set | 500,000 |
| Randomized L2 updates vs an ordered model | 600,000 |
| Randomized L3 commands vs a map + FIFO model | 1,600,000 |
| Exhaustive maker/taker/time-in-force combinations | 864 |
| Deterministic replay, pinned to a golden state hash | 100,000 |

Every operation compares reject reason, fill sequence, queue order, best bid and ask,
level aggregates, state hash and free-list integrity — exactly, not approximately.

| | Rust | C++ |
|---|---|---|
| Line coverage | 97.74% | 96.80% |
| Memory safety | `#![forbid(unsafe_code)]` | no owning raw pointers, no manual `new`/`delete` |
| Warnings | `clippy -D warnings` | `/W4 /WX`, `-Werror` |
| Sanitizers | — | ASan + UBSan via `-DBX_SANITIZERS=ON` |

Verified with MSVC 19.44 and Rust 1.97.1 on Windows 11. Detail in
[`docs/TESTING.md`](docs/TESTING.md).

## Layout

```text
rust/           Cargo crate — engine, 43 check groups, self-verifying benchmark
cpp/            Single header, optional C++ module, 4 test suites, benchmark
docs/           Architecture, testing contract, benchmark method
```

## Behavioral contract

Continuous, visible, single-instrument, price-time FIFO.

- Price priority precedes arrival priority; executions occur at resting-maker prices.
- An amend that only reduces quantity keeps its queue position. Any other change is a
  cancel/replace with a new ID and new priority.
- Post-only rejects a crossing order rather than repricing it.

Follows common exchange behavior as documented by CME, Coinbase Exchange and Nasdaq.

## Scope

A matching **core**, not an exchange. No gateway, account state, risk, journaling, market
data, auctions, hidden orders, sharding, networking or failover.
[**exchange-core**](https://github.com/hsdxpro/exchange-core) implements those around an
engine of this shape.

Decisions right for a bounded, verifiable engine and wrong for real order flow:

- Order IDs are dense table indices; quantities 32-bit, prices 16-bit ticks.
- No participant identity, so no self-trade prevention.
- Market orders sweep to the domain extreme with no collar.

[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) covers each and what changing it costs.

## License

[MIT](LICENSE)
