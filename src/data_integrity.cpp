#include "data_integrity.hpp"

#include <cmath>
#include <cstring>

namespace mokuren::integrity {

const char* CodeName(Code code) {
  switch (code) {
    case Code::Ok: return "ok";

    case Code::TradePriceNotFinite:    return "trade price is not finite";
    case Code::TradeQuantityNotFinite: return "trade quantity is not finite";
    case Code::TradePriceNotPositive:  return "trade price <= 0";
    case Code::TradeQuantityNegative:  return "trade quantity < 0";
    case Code::TradeQuantityZero:      return "trade quantity == 0";
    case Code::TradeTimestampNotPositive: return "trade timestamp <= 0";
    case Code::TradeTimestampBackwards:   return "trade timestamp went backwards";

    case Code::BookTimestampNotPositive: return "book timestamp <= 0";
    case Code::BookTimestampBackwards:   return "book timestamp went backwards";
    case Code::BookEmpty:                return "book has no levels";
    case Code::BookSideEmpty:            return "book has an empty side";
    case Code::BookTooShallow: return "book is shallower than the strategy needs";
    case Code::BookLevelPriceNotFinite:    return "book level price is not finite";
    case Code::BookLevelQuantityNotFinite: return "book level quantity is not finite";
    case Code::BookQuantityNegative:     return "book level quantity < 0";
    case Code::BookQuantityZero:         return "book level quantity == 0";
    case Code::BookBidsNotDescending:    return "bids are not strictly descending";
    case Code::BookAsksNotAscending:     return "asks are not strictly ascending";
    case Code::BookLocked:               return "book is locked (bid == ask)";
    case Code::BookCrossed:              return "book is crossed (bid > ask)";

    case Code::TransportError:   return "transport error";
    case Code::StreamEndedEarly: return "stream ended before the checks completed";
  }
  return "unknown violation";
}

Verdict ValidateTrade(const TradeEvent& t, int64_t& last_ts) {
  if (!std::isfinite(t.price)) return {Severity::Fatal, Code::TradePriceNotFinite};
  if (!std::isfinite(t.quantity))
    return {Severity::Fatal, Code::TradeQuantityNotFinite};
  if (t.price <= 0.0) return {Severity::Fatal, Code::TradePriceNotPositive};
  if (t.quantity < 0.0) return {Severity::Fatal, Code::TradeQuantityNegative};
  if (t.quantity == 0.0) return {Severity::Recoverable, Code::TradeQuantityZero};
  if (t.timestamp <= 0) return {Severity::Fatal, Code::TradeTimestampNotPositive};
  if (last_ts > 0 && t.timestamp < last_ts)
    return {Severity::Recoverable, Code::TradeTimestampBackwards};

  last_ts = t.timestamp;
  return {};
}

Verdict ValidateL2(const L2Snapshot& s, const BookLimits& limits,
                   int64_t& last_ts) {
  if (s.timestamp <= 0) return {Severity::Fatal, Code::BookTimestampNotPositive};
  if (last_ts > 0 && s.timestamp < last_ts) {
    return {limits.require_monotonic_timestamps ? Severity::Fatal
                                                : Severity::Recoverable,
            Code::BookTimestampBackwards};
  }

  Verdict worst;
  auto note = [&worst](Verdict v) {
    if (v.severity > worst.severity) worst = v;
  };

  // L2Snapshot has no per-side count: a side ends at the first non-positive
  // price, the remaining slots stay zeroed. That is the documented convention
  // both streamers build on, so the scan uses it as the terminator.
  size_t n_bid = 0;
  size_t n_ask = 0;
  double best_bid = 0.0;
  double best_ask = 0.0;
  double prev_bid = 0.0;
  double prev_ask = 0.0;

  for (size_t i = 0; i < static_cast<size_t>(MAX_L2_DEPTH); ++i) {
    const double px = s.bids[i].price;
    const double qty = s.bids[i].qty;
    if (!std::isfinite(px)) {
      note({Severity::Fatal, Code::BookLevelPriceNotFinite});
      break;
    }
    if (px <= 0.0) break;
    if (!std::isfinite(qty)) {
      note({Severity::Fatal, Code::BookLevelQuantityNotFinite});
      break;
    }
    if (qty < 0.0) {
      note({Severity::Fatal, Code::BookQuantityNegative});
      break;
    }
    if (qty == 0.0) note({Severity::Recoverable, Code::BookQuantityZero});
    // depth triggers (StackedImbalance, SpoofHunter) index these arrays by
    // position, so a mis-sorted side silently computes garbage instead of
    // failing. equal prices are a duplicated level, not a tie to break.
    if (n_bid > 0 && !(px < prev_bid))
      note({Severity::Fatal, Code::BookBidsNotDescending});
    if (n_bid == 0) best_bid = px;
    prev_bid = px;
    ++n_bid;
  }

  for (size_t i = 0; i < static_cast<size_t>(MAX_L2_DEPTH); ++i) {
    const double px = s.asks[i].price;
    const double qty = s.asks[i].qty;
    if (!std::isfinite(px)) {
      note({Severity::Fatal, Code::BookLevelPriceNotFinite});
      break;
    }
    if (px <= 0.0) break;
    if (!std::isfinite(qty)) {
      note({Severity::Fatal, Code::BookLevelQuantityNotFinite});
      break;
    }
    if (qty < 0.0) {
      note({Severity::Fatal, Code::BookQuantityNegative});
      break;
    }
    if (qty == 0.0) note({Severity::Recoverable, Code::BookQuantityZero});
    if (n_ask > 0 && !(px > prev_ask))
      note({Severity::Fatal, Code::BookAsksNotAscending});
    if (n_ask == 0) best_ask = px;
    prev_ask = px;
    ++n_ask;
  }

  if (n_bid == 0 && n_ask == 0) {
    note({Severity::Fatal, Code::BookEmpty});
  } else if (n_bid == 0 || n_ask == 0) {
    note({Severity::Fatal, Code::BookSideEmpty});
  } else if (best_bid > best_ask) {
    // mid-sweep reality on a fast venue, a parser bug on a slow one. either
    // way the frame is not trusted, but it does not by itself condemn the feed.
    note({Severity::Recoverable, Code::BookCrossed});
  } else if (best_bid == best_ask) {
    note({Severity::Recoverable, Code::BookLocked});
  }

  // checked after the structural rules so an empty or one-sided book is
  // reported as what it is rather than merely as shallow. a side that stops
  // early is not a thin market, it is a frame that cannot feed the depth
  // triggers, and there is no reading of it that is safe to pass on.
  if (limits.min_depth > 0 &&
      (n_bid < limits.min_depth || n_ask < limits.min_depth)) {
    note({Severity::Fatal, Code::BookTooShallow});
  }

  if (worst.severity == Severity::Ok) last_ts = s.timestamp;
  return worst;
}

bool IsBookGap(const L2Snapshot& s) {
  if (s.best_bid_price != 0.0 || s.best_ask_price != 0.0) return false;
  for (size_t i = 0; i < static_cast<size_t>(MAX_L2_DEPTH); ++i) {
    if (s.bids[i].price != 0.0 || s.bids[i].qty != 0.0) return false;
    if (s.asks[i].price != 0.0 || s.asks[i].qty != 0.0) return false;
  }
  return true;
}

L2Snapshot MakeBookGap(int64_t timestamp_ms) {
  L2Snapshot gap;
  std::memset(&gap, 0, sizeof(L2Snapshot));
  gap.timestamp = timestamp_ms;
  return gap;
}

}  // namespace mokuren::integrity
