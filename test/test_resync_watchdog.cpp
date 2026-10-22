// Unit tests for the backoff that decides when an unusable book justifies
// reconnecting the socket.
//
// The decision is pure arithmetic over a millisecond counter, which is the
// point: the alternative was a live socket and a stopwatch. What it must not
// do is hammer a venue that is already down, so the ceiling is tested as
// carefully as the first deadline.

#include "resync_watchdog.hpp"

#include <catch2/catch_test_macros.hpp>

using mokuren::ResyncWatchdog;

TEST_CASE("the watchdog stays quiet until the book is invalidated") {
  ResyncWatchdog w(1000, 8000);

  CHECK_FALSE(w.waiting());
  CHECK_FALSE(w.ShouldReconnect(0));
  CHECK_FALSE(w.ShouldReconnect(1000000));
}

TEST_CASE("the watchdog fires once the grace period has elapsed") {
  ResyncWatchdog w(1000, 8000);
  w.OnBookInvalidated(0);

  CHECK(w.waiting());
  CHECK(w.deadline_ms() == 1000);

  CHECK_FALSE(w.ShouldReconnect(999));
  CHECK(w.ShouldReconnect(1000));

  // const and non-consuming: the caller does the reconnect, then reports it.
  CHECK(w.ShouldReconnect(1001));
}

TEST_CASE("a reconnect attempt widens the wait, up to a ceiling") {
  ResyncWatchdog w(1000, 8000);
  w.OnBookInvalidated(0);

  CHECK(w.ShouldReconnect(1000));
  w.OnReconnectAttempted(1000);       // delay 2000
  CHECK(w.deadline_ms() == 3000);
  CHECK_FALSE(w.ShouldReconnect(2999));

  w.OnReconnectAttempted(3000);       // 4000
  CHECK(w.deadline_ms() == 7000);

  w.OnReconnectAttempted(7000);       // 8000
  CHECK(w.deadline_ms() == 15000);

  w.OnReconnectAttempted(15000);      // capped
  CHECK(w.deadline_ms() == 23000);
  w.OnReconnectAttempted(23000);      // still capped
  CHECK(w.deadline_ms() == 31000);
}

TEST_CASE("a landed snapshot resets the ladder") {
  ResyncWatchdog w(1000, 8000);
  w.OnBookInvalidated(0);
  w.OnReconnectAttempted(1000);
  w.OnReconnectAttempted(2000);
  w.OnReconnectAttempted(4000);

  w.OnSnapshotApplied();

  CHECK_FALSE(w.waiting());
  CHECK_FALSE(w.ShouldReconnect(1000000));

  // the next invalidation starts from the initial delay again: a blip must not
  // inherit a long backoff it did not earn.
  w.OnBookInvalidated(50000);
  CHECK(w.deadline_ms() == 51000);
}

TEST_CASE("attempts while not waiting are ignored") {
  ResyncWatchdog w(1000, 8000);
  w.OnReconnectAttempted(0);

  CHECK_FALSE(w.waiting());
  CHECK_FALSE(w.ShouldReconnect(999999));
}
