#pragma once

#include "data_integrity.hpp"

#include <cstdint>
#include <string>

namespace mokuren::integrity {

enum class ProbeState : uint8_t { Probing = 0, Ready = 1, Failed = 2 };

struct ChannelReport {
  std::string channel;  // "trades" | "l2"
  bool required = true;

  uint64_t seen = 0;         // every frame handed to the validator
  uint64_t delivered = 0;    // clean frames; the only ones the engine sees
  uint64_t recoverable = 0;  // rejected, not fatal (crossed book, zero qty)
  uint64_t fatal = 0;        // rejected, unrecoverable (NaN, mis-sorted book)

  Code first_recoverable = Code::Ok;
  Code first_fatal = Code::Ok;

  // the minimum this channel had to deliver to count as good, copied from the
  // gate's criteria so the report can answer "did this channel pass?" on its
  // own. zero for a channel the venue profile does not require.
  uint64_t min_required = 0;

  uint64_t rejected() const { return recoverable + fatal; }

  // "no fatal frames" is NOT the same as "this channel passed". a channel that
  // never delivered a single frame has zero fatals and would print ok with
  // 0 msgs -- the exact "pass by silence" the gate exists to reject. the count
  // has to clear its own minimum too, which is why min_required is carried
  // here instead of being left to the caller.
  bool passed() const {
    if (fatal > 0) return false;
    return !required || delivered >= min_required;
  }
};

struct ProbeReport {
  std::string venue;
  ChannelReport trades;
  ChannelReport l2;

  ProbeState state = ProbeState::Probing;
  uint32_t reconnects = 0;
  int64_t elapsed_ms = 0;
  std::string reason;  // empty on success

  bool passed() const { return state == ProbeState::Ready; }

  // one row per channel, aligned for a terminal:
  //   hyperliquid  trades  ok    (1284 msgs, 0 rejected)
  //   hyperliquid  l2      ok    (  96 msgs, 0 rejected)
  //   hyperliquid  gate    ok    (2013 ms)
  std::string ToString() const;
};

}  // namespace mokuren::integrity
