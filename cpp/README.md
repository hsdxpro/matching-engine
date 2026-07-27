# Bitmap Matching Engine — C++

The C++ implementation. A single dependency-free header, `bitmap_exchange.hpp`, holding the
bitmap ladder, the L2 aggregated book, and the L3 order-by-order book with its matching
engine. Nothing outside the standard library, nothing allocated after construction.

The design and the reasoning behind it are shared with the Rust implementation and live in
[`../docs/`](../docs/). This page covers what is specific to the C++ side.

## Build

Needs CMake 3.24 or newer and a C++23 compiler.

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Then the benchmark:

```bash
./build/Release/bx_bench
```

Use `./build/bx_bench` on a single-config generator such as Ninja or Makefiles. Add
`--quick` for a one-second run.

### Options

| Option | Default | Effect |
|---|---|---|
| `BX_NATIVE` | `ON` | `-march=native`, or `/arch:AVX2` on MSVC |
| `BX_IPO` | `ON` | Link-time optimization where the toolchain supports it |
| `BX_WARNINGS_AS_ERRORS` | `ON` | `/WX`, or `-Werror` |
| `BX_SANITIZERS` | `OFF` | AddressSanitizer and UndefinedBehaviorSanitizer |
| `BX_MODULE` | `OFF` | Also build and test the C++ module interface |

## On C++23, and on modules

Both were considered on the merits rather than adopted for the label, so it is worth being
direct about what is and is not used here.

**The engine is C++23 by standard setting and overwhelmingly C++20 by feature use.** What
it genuinely needs is `<bit>` — `countl_zero` and `countr_zero` are the bitmap's inner loop
— and `[[likely]]` / `[[unlikely]]` on the matching path. Of the headline C++23 additions,
exactly one earns its place: `std::to_underlying`, which replaces a `static_cast` whose
target type had to be kept in sync by hand.

The others were looked at and rejected for concrete reasons. `std::expected` does not fit,
because `SubmitResult` is not an either/or: an immediate-or-cancel order can partially fill
*and* be rejected, so it must carry fills and a reject reason at the same time. Forcing it
into `expected` would lose information. Deducing-this has nothing to simplify, because the
duplicated accessor names in this header belong to two different classes rather than to
const and non-const overloads of one.

Adding either would have grown the code to look modern while making it worse. That is the
opposite of the point.

**Modules are provided, but not as the only front door.** `bitmap_exchange.cppm` wraps the
header and exports the same names, and `-DBX_MODULE=ON` builds it along with
`tests/test_module.cpp`, which reaches the engine through `import` and never includes the
header — so a missing or incomplete export fails the build.

It is off by default for one reason: portability. MSVC's module support is solid and
Clang's is workable from 17, but GCC's remains incomplete enough that a modules-only
library would simply fail to build for a large share of anyone who cloned this. A header
that works everywhere beats a module that works impressively in some places. This is the
same arrangement `fmt` ships, and for the same reason.

## Tests

Four suites, run by `ctest`:

| Target | Covers |
|---|---|
| `bx_test_bitmap_l2` | Three-tier bitmap, L2 book, sparse traversal, VWAP |
| `bx_test_l3` | FIFO queues, matching, TIF semantics, amend, cancel, replace |
| `bx_test_differential` | Randomized comparison against an independent reference model |
| `bx_test_module` | The module interface, only when `BX_MODULE=ON` |

`bx_test_differential` is the one that carries the weight. It drives the engine and a model
built from `std::map` plus plain FIFO vectors — sharing no code with the engine — through
1.6 million randomized L3 commands, comparing reject reason, fill sequence, queue order,
best bid and ask, level aggregates, a state hash, and free-list integrity after every
single operation. It also runs 864 exhaustive maker/taker/time-in-force combinations and a
100,000-command deterministic replay pinned to a golden hash.

Engine-header line coverage is 96.80%. The uncovered remainder is invariant-failure
branches that can only be reached by deliberately corrupting private state.

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

## Measurements

Windows 11 desktop, MSVC 19.44 release build, no CPU pinning, three runs. Batch-normalized
throughput-equivalent service times over cache-resident working sets — not tail latency,
and not end-to-end exchange latency.

| Scenario | p50 | p99 | Per item |
|---|---:|---:|---:|
| L2 set level + cached BBO | 6.20 ns | 10.77 ns | 6.20 ns/update |
| L2 top 10 sparse levels | 48.44 ns | 65.51 ns | **4.84 ns/level** |
| L2 top 1,000 sparse levels | 4.94 µs | 6.15 µs | **4.94 ns/level** |
| L2 VWAP across 1,000 sparse levels | 4.18 µs | 5.28 µs | **4.17 ns/level** |
| L3 resting add + cached BBO | 7.08 ns | 11.51 ns | 7.08 ns/order |
| L3 same-price quantity reduction | 59.86 ns | 91.19 ns | 59.86 ns/order |
| L3 replace, loses FIFO priority | 158.54 ns | 313.38 ns | 158.54 ns/order |
| L3 direct-ID random cancel | 57.86 ns | 137.09 ns | 57.86 ns/order |
| L3 match 1 FIFO maker | 18.26 ns | 23.99 ns | **18.26 ns/fill** |
| L3 match 64 FIFO makers | 518.75 ns | 731.25 ns | **8.11 ns/fill** |
| L3 sweep 1,000 sparse levels | 18.90 µs | 23.30 µs | **18.90 ns/fill** |
| Mixed order-entry stream | 24.12 ns | 36.08 ns | 24.12 ns/message |

Read the per-item column. Walking 1,000 levels spread across the full 65,536-tick domain
costs 4.94 ns each against 4.84 ns for 10 adjacent levels — spreading the book out costs
almost nothing, which is exactly what the bitmap is for. Matching behaves the same way: one
fill costs 18.26 ns, sixty-four cost 8.11 ns each.

The reduce, replace and cancel rows visit resting orders in a **random permutation**, so
they carry cache misses that a sequential walk would not. That is deliberate — it is closer
to how a real book is amended — but it also means these three rows are not comparable
against the corresponding Rust rows, which walk in insertion order. See the note in the
[top-level README](../README.md#l3-book-and-matching-engine).

Method in [`../docs/BENCHMARKS.md`](../docs/BENCHMARKS.md).
