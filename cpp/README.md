# Matching Engine — C++

The C++23 implementation: a single dependency-free header holding the bitmap ladder, the L2
aggregated book, and the L3 order-by-order book with its matching engine.

Start at the [top-level README](../README.md) for the design and the cross-language
comparison. This page is the C++ specifics.

## Build

Needs CMake 3.24+ and a C++23 compiler.

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Benchmark: `./build/Release/bx_bench` — or `./build/bx_bench` on a single-config generator
such as Ninja. Add `--quick` for a one-second run.

| Option | Default | Effect |
|---|---|---|
| `BX_NATIVE` | `ON` | `-march=native`, or `/arch:AVX2` on MSVC |
| `BX_IPO` | `ON` | Link-time optimization where supported |
| `BX_WARNINGS_AS_ERRORS` | `ON` | `/WX`, or `-Werror` |
| `BX_SANITIZERS` | `OFF` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `BX_MODULE` | `OFF` | Also build and test the C++ module interface |

## Tests

```bash
ctest --test-dir build -C Release
```

| Target | Covers |
|---|---|
| `bx_test_bitmap_l2` | Three-tier bitmap, L2 book, sparse traversal, VWAP |
| `bx_test_l3` | FIFO queues, matching, TIF semantics, amend, cancel, replace |
| `bx_test_differential` | Randomized comparison against an independent reference model |
| `bx_test_module` | The module interface — only when `BX_MODULE=ON` |

`bx_test_differential` carries the weight. It drives the engine and a model built from
`std::map` plus plain FIFO vectors — sharing no code with the engine — through 1.6 million
randomized commands, comparing reject reason, fill sequence, queue order, best bid and ask,
level aggregates, a state hash and free-list integrity after **every** operation. Plus 864
exhaustive maker/taker/TIF combinations and a 100,000-command replay pinned to a golden
hash.

Engine-header line coverage is **96.80%**. The remainder is invariant-failure branches
reachable only by deliberately corrupting private state.

## Benchmarks

Windows 11, MSVC 19.44 release, no CPU pinning, three runs. Batch-normalized service times
over cache-resident data — not tail latency, not end-to-end exchange latency.

| Scenario | p50 | p99 | Per item |
|---|---:|---:|---:|
| L2 set level + cached BBO | 6.20 ns | 10.77 ns | 6.20 ns/update |
| L2 top 10 sparse levels | 48.44 ns | 65.51 ns | **4.84 ns/level** |
| L2 top 1,000 sparse levels | 4.94 µs | 6.15 µs | **4.94 ns/level** |
| L2 VWAP across 1,000 levels | 4.18 µs | 5.28 µs | **4.17 ns/level** |
| L3 resting add + cached BBO | 7.08 ns | 11.51 ns | 7.08 ns/order |
| L3 same-price reduction † | 59.86 ns | 91.19 ns | 59.86 ns/order |
| L3 replace, loses priority † | 158.54 ns | 313.38 ns | 158.54 ns/order |
| L3 direct-ID random cancel † | 57.86 ns | 137.09 ns | 57.86 ns/order |
| L3 match 1 FIFO maker | 18.26 ns | 23.99 ns | **18.26 ns/fill** |
| L3 match 64 FIFO makers | 518.75 ns | 731.25 ns | **8.11 ns/fill** |
| L3 sweep 1,000 sparse levels | 18.90 µs | 23.30 µs | **18.90 ns/fill** |
| Mixed order-entry stream | 24.12 ns | 36.08 ns | 24.12 ns/message |

Read the per-item column: 1,000 levels spread across the full domain cost 4.94 ns each
against 4.84 ns for 10 adjacent ones. One fill costs 18.26 ns, sixty-four cost 8.11 ns each.

† These three visit resting orders in a **random permutation**, so they carry cache misses a
sequential walk would not. Deliberate — it is closer to how a real book is amended — but it
means they are not comparable against the Rust rows, which walk in insertion order.

Method in [`../docs/BENCHMARKS.md`](../docs/BENCHMARKS.md).

## On C++23 and modules

Both were decided on merits rather than adopted for the label.

**The engine is C++23 by standard and overwhelmingly C++20 by feature use.** What it needs
is `<bit>` — `countl_zero` and `countr_zero` are the bitmap's inner loop — and
`[[likely]]`/`[[unlikely]]` on the matching path. Of the headline C++23 additions exactly
one earns its place: `std::to_underlying`, replacing a `static_cast` whose target type had
to be kept in sync by hand.

The others were examined and rejected:

- **`std::expected` does not fit.** `SubmitResult` is not an either/or — an
  immediate-or-cancel order can partially fill *and* be rejected, so it must carry fills and
  a reject reason together. Forcing it into `expected` would lose information.
- **Deducing-this has nothing to simplify.** The duplicated accessor names in this header
  belong to two different classes, not to const and non-const overloads of one.

Adding either would have grown the code to look modern while making it worse.

**Modules are provided, but not as the only front door.** `bitmap_exchange.cppm` wraps the
header and exports the same names; `-DBX_MODULE=ON` builds it plus `tests/test_module.cpp`,
which reaches the engine through `import` and never includes the header — so a missing
export fails the build.

Off by default for one reason: portability. MSVC's support is solid and Clang's is workable
from 17, but GCC's remains incomplete enough that a modules-only library would fail to
build for a large share of anyone who cloned this. A header that works everywhere beats a
module that works impressively in some places. Same arrangement `fmt` ships.

## Layout

```text
bitmap_exchange.hpp    The engine. One header, no dependencies.
bitmap_exchange.cppm   Optional module interface over the same header.
bench.cpp              Benchmark binary.
tests/
  test_support.hpp       Assertion macros, runner, deterministic RNG
  reference_model.hpp    std::map + FIFO model, independent of the engine
  test_bitmap_l2.cpp
  test_l3.cpp
  test_differential.cpp
  test_module.cpp        Built only when BX_MODULE=ON
```
