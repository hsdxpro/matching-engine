// A C++20 module interface over the same engine.
//
// Optional, and off by default. The reason is portability rather than taste:
// MSVC's module support is solid, Clang's is workable from 17, and GCC's is
// still incomplete enough that a modules-only library would fail to build for a
// large share of anyone who cloned this. A header that works everywhere beats a
// module that works impressively in some places.
//
// So the engine stays a single dependency-free header and this wraps it, which
// is the same arrangement `fmt` ships. Build with `-DBX_MODULE=ON` to compile
// and test through the module instead of the header.
//
// The global module fragment is what makes this legal: the standard library is
// included there, before the `export module` declaration, so its names are
// attached to the global module and not re-exported by this one. Consumers get
// `bitmap_exchange` and nothing else.

module;

#include "bitmap_exchange.hpp"

export module bitmap_exchange;

export namespace bitmap_exchange {

// Types.
using bitmap_exchange::Fill;
using bitmap_exchange::HierarchicalBitmap;
using bitmap_exchange::L2Book;
using bitmap_exchange::L3Book;
using bitmap_exchange::OrderSlot;
using bitmap_exchange::OrderView;
using bitmap_exchange::PriceLevel;
using bitmap_exchange::RejectReason;
using bitmap_exchange::Side;
using bitmap_exchange::SubmitResult;
using bitmap_exchange::SweepResult;
using bitmap_exchange::TimeInForce;

// Free functions and constants.
using bitmap_exchange::kInvalidIndex;
using bitmap_exchange::kMostAggressiveAskTick;
using bitmap_exchange::kMostAggressiveBidTick;
using bitmap_exchange::kNoAsk;
using bitmap_exchange::kNoBid;
using bitmap_exchange::kPriceCount;
using bitmap_exchange::mix64;
using bitmap_exchange::opposite;
using bitmap_exchange::side_index;
using bitmap_exchange::succeeded;
using bitmap_exchange::valid_time_in_force;

} // namespace bitmap_exchange
