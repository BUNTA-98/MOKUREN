// Unit tests for the data-integrity layer that sits between a streamer and the
// engine. Nothing here opens a socket: the checks, the gate and the report are
// all pure functions of structs and a millisecond counter, which is the whole
// point of keeping them out of the streamers.

#include "data_integrity.hpp"
#include "probe_report.hpp"
#include "startup_gate.hpp"
#include "venue_profile.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

using mokuren::integrity::BookLimits;
using mokuren::integrity::Code;
using mokuren::integrity::IsBookGap;
using mokuren::integrity::MakeBookGap;
using mokuren::integrity::ProbeCriteria;
using mokuren::integrity::ProbeReport;
using mokuren::integrity::ProbeState;
using mokuren::integrity::ProfileForVenue;
using mokuren::integrity::Severity;
using mokuren::integrity::StartupGate;
using mokuren::integrity::ValidateL2;
using mokuren::integrity::ValidateTrade;
using mokuren::integrity::VenueProfile;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

TradeEvent GoodTrade(int64_t ts = 1000, double price = 100.0, double qty = 1.0) {
  TradeEvent t{};
  t.timestamp = ts;
  t.price = price;
  t.quantity = qty;
  return t;
}

// depth 1: bids[0]/asks[0] populated and every following slot left zeroed,
// which is the end-of-side marker the validator scans for (L2Snapshot carries
// no per-side count, by design -- see trade_event.hpp).
L2Snapshot GoodBook(int64_t ts = 1000, double bid = 100.0, double ask = 100.5) {
  L2Snapshot s{};
  s.timestamp = ts;
  s.bids[0].price = bid;
  s.bids[0].qty = 1.0;
  s.asks[0].price = ask;
  s.asks[0].qty = 1.0;
  return s;
}

// minimums of 1 per channel and no soak time: fast enough for a unit test,
// same code path as the production criteria.
ProbeCriteria FastCriteria() {
  ProbeCriteria c;
  // a positive rate with a zero window still requires one frame per channel:
  // that floor is what these cases rely on, not the magnitude of the rate.
  c.min_trades_per_min = 60;
  c.min_l2_frames_per_min = 60;
  c.min_duration_ms = 0;
  c.timeout_ms = 10000;
  c.max_reconnects = 0;
  // the fixtures in this suite are depth-1 books, deliberately: these cases
  // are about the structural rules, and depth has its own cases above. the
  // shipped default is 2, which is what StackedImbalance indexes.
  c.min_l2_depth = 1;
  return c;
}

}  // namespace

// ---------------------------------------------------------------------------
// trade prints
// ---------------------------------------------------------------------------

TEST_CASE("a non-finite trade price is fatal") {
  int64_t cursor = 0;
  TradeEvent t = GoodTrade();
  t.price = kNaN;

  const auto v = ValidateTrade(t, cursor);
  REQUIRE(v.fatal());
  CHECK(v.code == Code::TradePriceNotFinite);
}

TEST_CASE("a zero-price trade is fatal") {
  int64_t cursor = 0;
  const auto v = ValidateTrade(GoodTrade(1000, 0.0), cursor);
  REQUIRE(v.fatal());
  CHECK(v.code == Code::TradePriceNotPositive);
}

TEST_CASE("a negative trade quantity is fatal") {
  int64_t cursor = 0;
  const auto v = ValidateTrade(GoodTrade(1000, 100.0, -1.0), cursor);
  REQUIRE(v.fatal());
  CHECK(v.code == Code::TradeQuantityNegative);
}

TEST_CASE("a well-formed print advances the cursor and passes") {
  int64_t cursor = 0;
  CHECK(ValidateTrade(GoodTrade(2000), cursor).ok());
  CHECK(cursor == 2000);
}

TEST_CASE("a backwards timestamp is recoverable, not fatal") {
  int64_t cursor = 0;
  REQUIRE(ValidateTrade(GoodTrade(2000), cursor).ok());

  const auto v = ValidateTrade(GoodTrade(1500), cursor);
  CHECK(v.severity == Severity::Recoverable);
  CHECK(v.code == Code::TradeTimestampBackwards);
  // a rejected print must not move the cursor backwards with it.
  CHECK(cursor == 2000);
}

// ---------------------------------------------------------------------------
// book snapshots
// ---------------------------------------------------------------------------

TEST_CASE("a well-formed book passes") {
  int64_t cursor = 0;
  CHECK(ValidateL2(GoodBook(), BookLimits{}, cursor).ok());
  CHECK(cursor == 1000);
}

TEST_CASE("a book meeting the required depth passes") {
  int64_t cursor = 0;
  BookLimits limits;
  limits.min_depth = 2;

  L2Snapshot s = GoodBook();
  s.bids[1].price = 99.5;
  s.bids[1].qty = 2.0;
  s.asks[1].price = 101.0;
  s.asks[1].qty = 2.0;

  CHECK(ValidateL2(s, limits, cursor).ok());
}

TEST_CASE("a book shallower than the required depth is fatal") {
  int64_t cursor = 0;
  BookLimits limits;
  limits.min_depth = 2;

  // GoodBook() is depth 1: structurally valid, but StackedImbalance reads
  // bids[1]/asks[1] and would compute a confident answer from zeroed slots.
  const auto v = ValidateL2(GoodBook(), limits, cursor);
  REQUIRE(v.fatal());
  CHECK(v.code == Code::BookTooShallow);
}

TEST_CASE("a crossed book is recoverable: market reality, not a parser bug") {
  int64_t cursor = 0;
  const auto v = ValidateL2(GoodBook(1000, 101.0, 100.0), BookLimits{}, cursor);
  CHECK(v.severity == Severity::Recoverable);
  CHECK(v.code == Code::BookCrossed);
}

TEST_CASE("a locked book is recoverable") {
  int64_t cursor = 0;
  const auto v = ValidateL2(GoodBook(1000, 100.0, 100.0), BookLimits{}, cursor);
  CHECK(v.severity == Severity::Recoverable);
  CHECK(v.code == Code::BookLocked);
}

TEST_CASE("a mis-sorted bid side is fatal: depth triggers index by position") {
  int64_t cursor = 0;
  L2Snapshot s = GoodBook();
  s.bids[1].price = 101.0;  // ascending inside a bid side
  s.bids[1].qty = 1.0;

  const auto v = ValidateL2(s, BookLimits{}, cursor);
  REQUIRE(v.fatal());
  CHECK(v.code == Code::BookBidsNotDescending);
}

TEST_CASE("a book with no levels at all is fatal") {
  int64_t cursor = 0;
  L2Snapshot s{};
  s.timestamp = 1000;

  const auto v = ValidateL2(s, BookLimits{}, cursor);
  REQUIRE(v.fatal());
  CHECK(v.code == Code::BookEmpty);
}

TEST_CASE("a book with one empty side is fatal") {
  int64_t cursor = 0;
  L2Snapshot s = GoodBook();
  s.asks[0] = L2Level{};

  const auto v = ValidateL2(s, BookLimits{}, cursor);
  REQUIRE(v.fatal());
  CHECK(v.code == Code::BookSideEmpty);
}

TEST_CASE("a non-finite level price is fatal") {
  int64_t cursor = 0;
  L2Snapshot s = GoodBook();
  s.asks[0].price = kNaN;

  const auto v = ValidateL2(s, BookLimits{}, cursor);
  REQUIRE(v.fatal());
  CHECK(v.code == Code::BookLevelPriceNotFinite);
}

TEST_CASE("the gap sentinel is recognised by shape and is not a tradeable book") {
  CHECK(IsBookGap(MakeBookGap(123)));
  CHECK_FALSE(IsBookGap(GoodBook()));
}

// ---------------------------------------------------------------------------
// startup gate -- "only pass if both are correct"
// ---------------------------------------------------------------------------

TEST_CASE("the gate arms only once every required channel is well-formed") {
  StartupGate gate("testvenue", VenueProfile{}, FastCriteria(), 0);

  CHECK(gate.ObserveTrade(GoodTrade(1000), 0) == ProbeState::Probing);
  CHECK(gate.ObserveL2(GoodBook(1000), 0) == ProbeState::Ready);
  CHECK(gate.Report().passed());
}

TEST_CASE("perfect trades plus a malformed book fails the gate") {
  StartupGate gate("testvenue", VenueProfile{}, FastCriteria(), 0);

  for (int i = 0; i < 5; ++i) {
    CHECK(gate.ObserveTrade(GoodTrade(1000 + i), 0) == ProbeState::Probing);
  }

  L2Snapshot broken = GoodBook();
  broken.asks[0].price = kNaN;
  CHECK(gate.ObserveL2(broken, 0) == ProbeState::Failed);

  const ProbeReport report = gate.Report();
  CHECK(report.trades.passed());    // the trades channel was perfectly fine
  CHECK_FALSE(report.l2.passed());  // and it still refuses to arm
  CHECK_FALSE(report.passed());
  CHECK(report.l2.first_fatal == Code::BookLevelPriceNotFinite);
}

TEST_CASE("perfect book plus malformed trades fails the gate") {
  StartupGate gate("testvenue", VenueProfile{}, FastCriteria(), 0);

  CHECK(gate.ObserveL2(GoodBook(1000), 0) == ProbeState::Probing);
  CHECK(gate.ObserveTrade(GoodTrade(1000, 0.0), 0) == ProbeState::Failed);

  const ProbeReport report = gate.Report();
  CHECK(report.l2.passed());
  CHECK_FALSE(report.trades.passed());
}

TEST_CASE("a silent channel fails on the deadline instead of passing by silence") {
  ProbeCriteria criteria = FastCriteria();
  criteria.timeout_ms = 100;
  StartupGate gate("testvenue", VenueProfile{}, criteria, 0);

  CHECK(gate.ObserveTrade(GoodTrade(1000), 0) == ProbeState::Probing);
  CHECK(gate.Poll(50) == ProbeState::Probing);
  CHECK(gate.Poll(100) == ProbeState::Failed);

  const ProbeReport report = gate.Report();
  CHECK_FALSE(report.l2.passed());
  CHECK(report.reason.find("l2") != std::string::npos);
}

TEST_CASE("a reconnect during the checks fails a feed that promised stability") {
  StartupGate gate("testvenue", VenueProfile{}, FastCriteria(), 0);

  CHECK(gate.ObserveTrade(GoodTrade(1000), 0) == ProbeState::Probing);
  CHECK(gate.ObserveBookGap(0) == ProbeState::Failed);
}

TEST_CASE("a clean stream end before the minimums is a failure, not a timeout") {
  StartupGate gate("testvenue", VenueProfile{}, FastCriteria(), 0);

  CHECK(gate.ObserveTrade(GoodTrade(1000), 0) == ProbeState::Probing);
  CHECK(gate.ObserveStreamEnded(100) == ProbeState::Failed);
  CHECK(gate.Report().reason == "stream ended before the checks completed");
}

TEST_CASE("a transport error during the checks fails immediately") {
  StartupGate gate("testvenue", VenueProfile{}, FastCriteria(), 0);
  CHECK(gate.ObserveTransportError("socket closed", 10) == ProbeState::Failed);
  CHECK(gate.Report().reason.find("socket closed") != std::string::npos);
}

TEST_CASE("the report prints one row per channel plus the verdict") {
  StartupGate gate("hyperliquid", VenueProfile{}, FastCriteria(), 0);
  gate.ObserveTrade(GoodTrade(1000), 0);
  gate.ObserveL2(GoodBook(1000), 0);

  const std::string text = gate.Report().ToString();
  CHECK(text.find("hyperliquid") != std::string::npos);
  CHECK(text.find("trades") != std::string::npos);
  CHECK(text.find("l2") != std::string::npos);
  CHECK(text.find("ok") != std::string::npos);
}

// ---------------------------------------------------------------------------
// venue profiles
// ---------------------------------------------------------------------------

TEST_CASE("the disabled criteria arm on the first poll: the ungated Start path") {
  StartupGate gate("testvenue", VenueProfile{},
                   mokuren::integrity::ProbeCriteria::Disabled(), 0);
  // this is what LiveOrchestrator::Start() relies on: zero minimums and zero
  // soak mean Ready at elapsed 0, without waiting for a frame. if this ever
  // stops holding, the plain Start() silently becomes a 20s stall.
  CHECK(gate.Poll(0) == ProbeState::Ready);
}

TEST_CASE("an undeclared depth cannot trip the capability pre-check") {
  // l2_depth == 0 means "not declared", not "zero levels". the gate has
  // nothing to compare, so the venue must reach the frame-level rules rather
  // than being rejected before it has sent anything.
  VenueProfile undeclared;  // l2_depth stays 0
  undeclared.venue = "vendorx";

  ProbeCriteria criteria = FastCriteria();
  criteria.min_l2_depth = 20;

  StartupGate gate("vendorx", undeclared, criteria, 0);

  CHECK(gate.Poll(0) == ProbeState::Probing);
}

TEST_CASE("an unknown venue gets the strict profile") {
  const auto profile = ProfileForVenue("some-exchange-added-next-year");
  CHECK(profile.trades_required);
  CHECK(profile.l2_required);
  CHECK_FALSE(profile.has_sequence_numbers);
  // undeclared depth: the frame-level check still runs, but the gate cannot
  // claim the feed is incapable before it has seen anything.
  CHECK(profile.l2_depth == 0);
}

TEST_CASE("bybit declares sequencing, hyperliquid does not") {
  CHECK(ProfileForVenue("bybit").has_sequence_numbers);
  CHECK_FALSE(ProfileForVenue("hyperliquid").has_sequence_numbers);
}

TEST_CASE("the declared book depth reflects each venue's subscription") {
  // bybit subscribes to orderbook.50 and reconciles it into a full snapshot;
  // the profile declares the depth post-truncation, so it is capped by what
  // the engine can actually store. hyperliquid pushes a 20-level book.
  const size_t cap = static_cast<size_t>(MAX_L2_DEPTH);
  const size_t expected_bybit = 50 < cap ? 50 : cap;

  CHECK(ProfileForVenue("bybit").l2_depth == expected_bybit);
  CHECK(ProfileForVenue("hyperliquid").l2_depth == 20);
}

TEST_CASE("bybit's declared depth clears the default requirement") {
  // the inversion of the capability pre-check, and the regression this suite
  // exists to catch: if the subscription ever drops back to orderbook.1 the
  // profile must say so, and the default min_l2_depth of 2 must reject it.
  // without this case, a silent downgrade would starve the depth triggers with
  // every other test still green.
  const VenueProfile bybit = ProfileForVenue("bybit");
  const mokuren::integrity::ProbeCriteria defaults{};

  CHECK(bybit.l2_depth >= defaults.min_l2_depth);
}

TEST_CASE("a venue that cannot publish the required depth fails before any frame") {
  // a synthetic profile, not bybit: this case is about the gate's capability
  // rule, and binding it to a real venue made it break every time a
  // subscription changed while testing none of the rule's own edge cases.
  VenueProfile shallow;
  shallow.venue = "vendorx";
  shallow.l2_depth = 2;

  ProbeCriteria criteria = FastCriteria();
  criteria.min_l2_depth = 20;

  StartupGate gate("vendorx", shallow, criteria, 0);

  // decidable from the profile alone: two levels per side can never satisfy a
  // twenty-level requirement. failing on the first poll beats spending the
  // whole soak accepting frames whose level 3 onwards is a zeroed slot.
  CHECK(gate.Poll(0) == ProbeState::Failed);

  const ProbeReport report = gate.Report();
  CHECK_FALSE(report.passed());
  CHECK(report.reason.find("publishes 2") != std::string::npos);
  CHECK(report.reason.find("needs 20") != std::string::npos);
}
