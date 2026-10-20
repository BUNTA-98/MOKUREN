#pragma once

#include <cstdint>

// CSV exports arrive in seconds, milliseconds, microseconds or nanoseconds
// depending on the venue and the export tool, while the engine only ever
// reasons in milliseconds. Normalise once, here, at the ingest boundary: a
// microsecond export used to pass the "> 1.5e12" plausibility check and was
// then read as milliseconds, dating every row to the year 55000.
inline int64_t NormalizeEpochToMillis(int64_t ts) {
  if (ts <= 0) return ts;

  // nanoseconds since epoch: ~1.7e18 today. 1e17 ns is 1973.
  if (ts >= 100'000'000'000'000'000LL) return ts / 1'000'000;

  // microseconds since epoch: ~1.7e15 today. 1e14 ms would be the year 5138,
  // so nothing in this range can legitimately be milliseconds.
  if (ts >= 100'000'000'000'000LL) return ts / 1'000;

  // seconds since epoch: ~1.7e9 today, and 1e11 ms is 1973.
  if (ts < 100'000'000'000LL) return ts * 1'000;

  return ts;
}
