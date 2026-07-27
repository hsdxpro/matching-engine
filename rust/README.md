# Matching Engine — Rust

The Rust implementation: a single-instrument order book aggregated by price (L2) and by
order (L3), with a price-time FIFO matching engine. `#![forbid(unsafe_code)]`, no
third-party crates, no allocation after construction.

Start at the [top-level README](../README.md) for the design and the cross-language
comparison. This page is the Rust specifics.

## Build

```bash
cargo run --release --bin bx-bench
```

That is the whole thing. It runs all 43 verification groups, prints each as `PASS` or
`FAIL`, and only then measures. A failed check aborts before any number is reported —
a benchmark from an engine that fails a correctness check is worthless.

Add `--quick` for a one-second run, or `--bench-only` to skip verification.

Needs [rustup](https://rustup.rs) and nothing else. `rust-toolchain.toml` pins Rust 1.97.1
and rustup fetches it on first use; there are no dependencies to download.

## Tests

```bash
cargo fmt --all -- --check
cargo clippy --all-targets --locked -- -D warnings
cargo test --release --locked
```

44 tests: the 43 groups individually, plus a check that every group is registered exactly
once.

The suite lives in [`src/verify/`](src/verify/) and is compiled into the library rather than
hidden behind `cfg(test)`, so the shipped binary and `cargo test` execute identical code.

| Check | Volume |
|---|---:|
| Named check groups | 43 |
| Randomized bitmap ops vs an ordered set | 500,000 |
| Randomized L2 updates vs an ordered model | 600,000 |
| Randomized L3 commands vs a map + FIFO model | 1,600,000 |
| Exhaustive maker/taker/TIF combinations | 864 |
| Deterministic replay, pinned to a golden hash | 100,000 |
| Engine line coverage | 97.74% |

The randomized groups are differential tests against reference models built from completely
different primitives — `BTreeMap` plus plain FIFO vectors, against the engine's bitmap
ladder over a fixed slot arena. Agreement is evidence both are right; it does not define
what right means. Every operation compares reject reason, fill sequence, queue order, BBO,
level aggregates, state hash and free-list integrity exactly.

## Benchmarks

Windows 11, Rust 1.97.1 release, no CPU pinning, three runs. Batch-normalized service times
over cache-resident data — not tail latency, not end-to-end exchange latency.

| Scenario | p50 | Per item |
|---|---:|---:|
| L2 set level + cached BBO | 6.98 ns | 6.98 ns/update |
| L2 top 10 sparse levels | 47.66 ns | **4.77 ns/level** |
| L2 top 1,000 sparse levels | 5.03 µs | **5.03 ns/level** |
| L2 VWAP across 1,000 levels | 4.42 µs | **4.42 ns/level** |
| L3 passive insert + BBO read | 11.08 ns | 11.08 ns/order |
| L3 amend down, priority retained † | 7.37 ns | 7.37 ns/order |
| L3 cancel/replace, new priority † | 36.38 ns | 36.38 ns/order |
| L3 cancel by order ID † | 10.50 ns | 10.50 ns/order |
| L3 aggress 1 resting maker | 20.90 ns | **20.90 ns/fill** |
| L3 aggress 64 resting makers | 740.62 ns | **11.57 ns/fill** |
| L3 sweep 1,000 sparse levels | 23.60 µs | **23.60 ns/fill** |
| Mixed order-entry stream | 51.22 ns | 51.22 ns/message |

Read the per-item column: 1,000 levels spread across the full 65,536-tick domain cost
5.03 ns each against 4.77 ns for 10 adjacent ones. One fill costs 20.90 ns, sixty-four cost
11.57 ns each.

† These three walk resting orders in **insertion order**, so they are cache-friendly in a
way the corresponding C++ rows are not — those visit a random permutation. The rows are not
comparable across languages.

Method in [`../docs/BENCHMARKS.md`](../docs/BENCHMARKS.md).

## What it demonstrates

- Hierarchical bitmap best-price discovery and sparse next/previous traversal
- Cached best bid/ask with constant-time reads
- L2 update, remove, top-N depth, sweep and VWAP
- L3 strict price-time FIFO queues
- GTC, IOC, FOK, post-only and market-order behavior
- Maker-price execution, partial fills, multi-fill and multi-level sweeps
- Direct order-ID lookup for cancel, amend and cancel/replace
- Amend-down retaining priority; cancel/replace assigning a new ID and new priority
- Atomic FOK, post-only, validation and capacity rejection
- Fixed-capacity, allocation-free operation after construction
- Compact 24-byte order slots and 16-byte level descriptors

## Layout

```text
src/lib.rs           The engine
src/verify/          43 check groups and the independent reference models
  mod.rs               Registry, runner, workload scaling
  reference.rs         BTreeMap/FIFO models sharing no code with the engine
  bitmap_l2.rs         Bitmap and L2 groups
  l3.rs                L3 and matching-engine groups
  differential.rs      Randomized, exhaustive and replay groups
  api.rs               Public API surface and error formatting
src/bin/bench.rs     Self-verifying benchmark binary
```
