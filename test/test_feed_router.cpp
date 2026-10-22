// End-to-end tests for the piece that actually gates the pipeline: a real
// IMarketStreamer pushes frames through a FeedRouter into a sink that stands in
// for the orchestrator. This is the layer the unit tests in
// test_data_integrity.cpp cannot reach -- there the gate is fed directly, here
// it is wired exactly as production wires it, so "the engine never sees a frame
// before the checks pass" is a property of the code and not of a comment.

#include "data_integrity.hpp"
#include "feed_router.hpp"
#include "live_streamer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using mokuren::integrity::FeedRouter;
using mokuren::integrity::IsBookGap;
using mokuren::integrity::LiveSink;
using mokuren::integrity::ProbeCriteria;
using mokuren::integrity::ProbeState;
using mokuren::integrity::VenueProfile;

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// stands in for the real websocket client: Start()/Stop() are no-ops and the
// test pushes frames explicitly, which is what a streamer does on its own
// thread.
class FakeStreamer : public IMarketStreamer {
 public:
  void Start() override {}
  void Stop() override {}

  void PushTrade(const TradeEvent& t) { EmitTrade(t); }
  void PushL2(const L2Snapshot& s) { EmitL2(s); }
  void Finish() { EmitFinished(); }
  void Fail(const std::string& message) { EmitError(message); }
};

// the engine side. records what actually reached the orchestrator.
struct Sink {
  std::vector<TradeEvent> trades;
  std::vector<L2Snapshot> books;
  std::vector<std::string> errors;
  int finished = 0;

  LiveSink Build() {
    LiveSink sink;
    sink.on_trade = [this](const TradeEvent& t) { trades.push_back(t); };
    sink.on_l2 = [this](const L2Snapshot& s) { books.push_back(s); };
    sink.on_finished = [this]() { ++finished; };
    sink.on_error = [this](const std::string& m) { errors.push_back(m); };
    return sink;
  }
};

TradeEvent GoodTrade(int64_t ts) {
  TradeEvent t{};
  t.timestamp = ts;
  t.price = 100.0;
  t.quantity = 1.0;
  return t;
}

L2Snapshot GoodBook(int64_t ts) {
  L2Snapshot s{};
  s.timestamp = ts;
  s.bids[0].price = 100.0;
  s.bids[0].qty = 1.0;
  s.asks[0].price = 100.5;
  s.asks[0].qty = 1.0;
  s.best_bid_price = 100.0;
  s.best_bid_qty = 1.0;
  s.best_ask_price = 100.5;
  s.best_ask_qty = 1.0;
  return s;
}

// one frame per channel is enough, and no soak time: the production path with
// the thresholds turned all the way down.
ProbeCriteria FastCriteria() {
  ProbeCriteria c;
  c.min_trades = 1;
  c.min_l2_frames = 1;
  c.min_duration_ms = 0;
  c.timeout_ms = 10000;
  c.max_reconnects = 0;
  // depth-1 fixtures, deliberately: the depth rule has its own case below.
  c.min_l2_depth = 1;
  return c;
}

// the router stamps its own observations from the wall clock, so the gate's
// start must come from the same clock or the deadline fires immediately.
// NOTE: that coupling is a wart -- the gate takes an injected clock, the router
// does not. wiring a clock through FeedRouter removes the need for this
// constant in every test; see the follow-up in the reply.
int64_t StartMs() { return NowMillis(); }

}  // namespace

TEST_CASE("the router withholds every frame until both channels are clean") {
  FakeStreamer streamer;
  const int64_t start = StartMs();
  FeedRouter router("testvenue", VenueProfile{}, FastCriteria(), start);

  Sink sink;
  router.SetLiveSink(sink.Build());
  router.Attach(streamer);

  streamer.PushTrade(GoodTrade(start));
  CHECK(sink.trades.empty());  // probing: counted, not forwarded

  streamer.PushL2(GoodBook(start));
  CHECK(router.Report().passed());
  CHECK(sink.trades.empty());  // still nothing forwarded, not even the frame
  CHECK(sink.books.empty());   // that satisfied the gate

  REQUIRE(router.BeginRunning(NowMillis()));

  // the engine must be told the book it never saw is not trustworthy, so it
  // waits for a fresh snapshot instead of trading a book built from zero frames.
  REQUIRE(sink.books.size() == 1);
  CHECK(IsBookGap(sink.books[0]));

  streamer.PushTrade(GoodTrade(start + 10));
  REQUIRE(sink.trades.size() == 1);
  CHECK(sink.trades[0].timestamp == start + 10);
}

TEST_CASE("a malformed book keeps the engine unarmed however good the trades") {
  FakeStreamer streamer;
  const int64_t start = StartMs();
  FeedRouter router("testvenue", VenueProfile{}, FastCriteria(), start);

  Sink sink;
  router.SetLiveSink(sink.Build());
  router.Attach(streamer);

  for (int i = 0; i < 5; ++i) streamer.PushTrade(GoodTrade(start + i));

  L2Snapshot broken = GoodBook(start);
  broken.asks[0].price = kNaN;
  streamer.PushL2(broken);

  CHECK(router.Report().state == ProbeState::Failed);
  CHECK_FALSE(router.BeginRunning(NowMillis()));
  CHECK(sink.trades.empty());
  CHECK(sink.books.empty());  // no sentinel either: the engine is never armed
}

TEST_CASE("a venue that drops during the checks never arms") {
  FakeStreamer streamer;
  const int64_t start = StartMs();
  FeedRouter router("testvenue", VenueProfile{}, FastCriteria(), start);

  Sink sink;
  router.SetLiveSink(sink.Build());
  router.Attach(streamer);

  streamer.PushTrade(GoodTrade(start));
  streamer.Fail("socket closed");

  CHECK(router.Report().state == ProbeState::Failed);
  CHECK(router.Report().reason.find("socket closed") != std::string::npos);
  CHECK_FALSE(router.BeginRunning(NowMillis()));
}

// NOTE: Catch2 registers test cases by name across every translation unit, so
// this must not reuse the gate-level case name in test_data_integrity.cpp
// ("a reconnect during the checks fails a feed that promised stability"). the
// duplicate was a hard error at startup and meant no test ran at all. the tag
// keeps the router suite filterable: ./build/engine_tests "[router]".
TEST_CASE("a reconnect during the checks never arms the router",
          "[router]") {
  FakeStreamer streamer;
  const int64_t start = StartMs();
  FeedRouter router("testvenue", VenueProfile{}, FastCriteria(), start);

  Sink sink;
  router.SetLiveSink(sink.Build());
  router.Attach(streamer);

  streamer.PushTrade(GoodTrade(start));  // delivered > 0, so a gap counts
  streamer.PushL2(mokuren::integrity::MakeBookGap(start + 1));

  CHECK(router.Report().state == ProbeState::Failed);
  CHECK(router.Report().reconnects == 1);
}

TEST_CASE("a fatal frame after arming stops the run instead of feeding it") {
  FakeStreamer streamer;
  const int64_t start = StartMs();
  FeedRouter router("testvenue", VenueProfile{}, FastCriteria(), start);

  Sink sink;
  router.SetLiveSink(sink.Build());
  router.Attach(streamer);

  streamer.PushTrade(GoodTrade(start));
  streamer.PushL2(GoodBook(start));
  REQUIRE(router.BeginRunning(NowMillis()));

  TradeEvent bad = GoodTrade(start + 20);
  bad.price = kNaN;
  streamer.PushTrade(bad);

  CHECK(sink.trades.empty());  // the bad print was not forwarded
  REQUIRE(sink.errors.size() == 1);
  CHECK(sink.errors[0].find("trades") != std::string::npos);

  // escalate once, not once per bad frame: a venue-wide field rename would
  // otherwise fill the error path with duplicate reports.
  streamer.PushTrade(bad);
  CHECK(sink.errors.size() == 1);
}

TEST_CASE("a book that loses depth after arming stops the run", "[router]") {
  FakeStreamer streamer;
  const int64_t start = StartMs();
  ProbeCriteria criteria = FastCriteria();
  criteria.min_l2_depth = 2;  // as the config ships

  FeedRouter router("testvenue", VenueProfile{}, criteria, start);

  Sink sink;
  router.SetLiveSink(sink.Build());
  router.Attach(streamer);

  L2Snapshot deep = GoodBook(start);
  deep.bids[1].price = 99.5;
  deep.bids[1].qty = 2.0;
  deep.asks[1].price = 101.0;
  deep.asks[1].qty = 2.0;

  streamer.PushTrade(GoodTrade(start));
  streamer.PushL2(deep);
  REQUIRE(router.BeginRunning(NowMillis()));
  REQUIRE(sink.books.size() == 1);  // the gap sentinel

  // the same venue, one level per side. this proves the running path inherits
  // the depth requirement from the criteria instead of only the probe doing so.
  streamer.PushL2(GoodBook(start + 10));

  CHECK(sink.books.size() == 1);  // the shallow frame was not forwarded
  REQUIRE(sink.errors.size() == 1);
  CHECK(sink.errors[0].find("l2") != std::string::npos);
}

TEST_CASE("a recoverable frame after arming is dropped and counted") {
  FakeStreamer streamer;
  const int64_t start = StartMs();
  FeedRouter router("testvenue", VenueProfile{}, FastCriteria(), start);

  Sink sink;
  router.SetLiveSink(sink.Build());
  router.Attach(streamer);

  streamer.PushTrade(GoodTrade(start));
  streamer.PushL2(GoodBook(start));
  REQUIRE(router.BeginRunning(NowMillis()));
  REQUIRE(sink.books.size() == 1);  // the gap sentinel

  L2Snapshot crossed = GoodBook(start + 10);
  crossed.bids[0].price = 101.0;  // above the ask
  streamer.PushL2(crossed);

  // a crossed book mid-sweep must not regress the local book, and it must not
  // end the run either: drop it, count it, keep the last good snapshot.
  CHECK(sink.books.size() == 1);
  CHECK(sink.errors.empty());
  CHECK(router.DroppedWhileRunning() == 1);
}
