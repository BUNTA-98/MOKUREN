#include "hyperliquid_streamer.hpp"

#include "data_integrity.hpp"
#include "live_debug.hpp"
#include "logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

namespace {

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// hyperliquid sends prices and sizes as decimal strings, but numbers appear on
// some channels -- accept both instead of throwing on one of them.
double ToDouble(const nlohmann::json& v) {
  if (v.is_string()) return std::stod(v.get<std::string>());
  if (v.is_number()) return v.get<double>();
  throw std::runtime_error("hyperliquid: expected a numeric field");
}

// a zeroed snapshot is the "book gap" sentinel, same contract as
// BybitDataStreamer's: the FeedRouter treats a frame with no bid or no ask as
// "the local book is no longer a faithful picture of the venue" and suppresses
// new entries until a real snapshot arrives. emitted on reconnect and on an
// unexpected close/error, because the socket recovers on its own and the worker
// would otherwise evaluate L2 triggers against a stale book in between.
//
// the shape is owned by mokuren::integrity (IsBookGap), not by this file: both
// producers must emit the identical sentinel, or the consumer recognises one
// venue's drop and sleeps through the other's.
L2Snapshot MakeBookGap() { return mokuren::integrity::MakeBookGap(NowMillis()); }

}  // namespace

HyperliquidDataStreamer::HyperliquidDataStreamer(const std::string& symbol,
                                                 const std::string& url)
    : symbol_(symbol), url_(url) {
  Connect();
}

HyperliquidDataStreamer::~HyperliquidDataStreamer() {
  // stop() joins the websocket thread before members are destroyed.
  Stop();
}

void HyperliquidDataStreamer::Start() {
  if (running_.exchange(true)) {
    LOG_WARN("HyperliquidDataStreamer::Start called while already running");
    return;
  }
  ws_.start();
}

void HyperliquidDataStreamer::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  LOG_INFO("HyperliquidDataStreamer: stopping websocket");
  ws_.stop();

  // One line that answers "quiet venue or dropped frames" without a grep.
  // received > emitted means HandleOrderBook refused frames and the LIVE_DBG
  // lines name the check; received == emitted means the venue really pushes
  // books this rarely and the recording is the whole truth. LOG_INFO, so it
  // reaches the console exactly when --debug is on -- the only time the
  // question is being asked.
  // counts FRAMES, not events: one "trades" frame carries an array of fills, so
  // this reads 5 where the gate reports 42 msgs for the same window. both are
  // right; they count different things, and comparing them without that note
  // looks like a lost-frames bug.
  LOG_INFO("hyperliquid {}: frames received l2Book={} trades={} other={}, "
           "l2 emitted={}",
           symbol_, frames_l2book_.load(std::memory_order_relaxed),
           frames_trades_.load(std::memory_order_relaxed),
           frames_other_.load(std::memory_order_relaxed),
           l2_emitted_.load(std::memory_order_relaxed));
}

void HyperliquidDataStreamer::Connect() {
  ws_.setUrl(url_);

  // survive transient drops without operator intervention. the library
  // reconnects on its own and re-fires Open, which re-sends the subscription.
  // ixwebsocket's ping is protocol-level, so no app-level heartbeat thread is
  // needed here (unlike BybitDataStreamer, which must send {"op":"ping"}).
  ws_.enableAutomaticReconnection();
  ws_.setPingInterval(30);

  // first_open is captured by value and mutated by the callback: the first
  // Open is the initial handshake (no gap to report), every subsequent Open is
  // a reconnect and must be preceded by a book-gap frame so the worker stops
  // trusting the local book until a real snapshot arrives.
  ws_.setOnMessageCallback(
      [this, first_open = true](const ix::WebSocketMessagePtr& msg) mutable {
    switch (msg->type) {
      case ix::WebSocketMessageType::Open:
        LOG_INFO(">>> HYPERLIQUID WEBSOCKET OPENED <<<");
        // a reconnect is only a gap if the drop has not already been signalled:
        // the handlers below see Error and Close before this, and emitting on
        // all three turns one outage into three reconnects.
        if (!first_open &&
            !gap_reported_.exchange(true, std::memory_order_relaxed)) {
          // reconnect: the local book is stale until the next l2Book snapshot.
          EmitL2(MakeBookGap());
        }
        first_open = false;
        connected_ = true;
        // the subscription is only valid once the handshake completed, and it
        // must be re-sent on every Open: the venue does not remember a
        // subscription across a new socket.
        SendSubscription();
        break;
      case ix::WebSocketMessageType::Close:
        LOG_WARN(">>> HYPERLIQUID WEBSOCKET CLOSED (code={}, reason={})",
                 msg->closeInfo.code, msg->closeInfo.reason);
        connected_ = false;
        // running_ is cleared by Stop() before ws_.stop(), so a close observed
        // while it is still set was not asked for. report a gap, not an error:
        // the socket will reconnect. at most once per outage -- see
        // gap_reported_ in the header.
        if (running_.load(std::memory_order_acquire) &&
            !gap_reported_.exchange(true, std::memory_order_relaxed)) {
          EmitL2(MakeBookGap());
        }
        break;
      case ix::WebSocketMessageType::Error:
        LOG_ERROR(">>> HYPERLIQUID WEBSOCKET ERROR ({}): {} <<<",
                  msg->errorInfo.http_status, msg->errorInfo.reason);
        connected_ = false;
        if (running_.load(std::memory_order_acquire) &&
            !gap_reported_.exchange(true, std::memory_order_relaxed)) {
          EmitL2(MakeBookGap());
        }
        break;
      case ix::WebSocketMessageType::Message:
        HandleMessage(msg->str);
        break;
      default:
        break;
    }
  });
}

void HyperliquidDataStreamer::SendSubscription() {
  // built from symbol_ so a non-BTC instance subscribes to its own asset.
  //
  // two frames, one per channel: "subscription" accepts a single object. the
  // trades feed is not optional -- the bar aggregator only advances on
  // TradeEvents, so an l2Book-only session closes no candle and evaluates no
  // trigger, which is indistinguishable from a quiet market.
  //
  // NOTE: nlohmann orders object keys alphabetically, so the emitted string is
  //   {"method":"subscribe","subscription":{"coin":"BTC","type":"l2Book"}}
  // which differs cosmetically from the hand-written literal but is the same
  // JSON document.
  for (const char* type : {"l2Book", "trades"}) {
    nlohmann::json sub;
    sub["method"] = "subscribe";
    sub["subscription"]["type"] = type;
    sub["subscription"]["coin"] = symbol_;

    const std::string payload = sub.dump();
    LIVE_DBG("hyperliquid: subscribing with {}", payload);
    ws_.send(payload);
  }
}

void HyperliquidDataStreamer::HandleMessage(const std::string& payload) {
  LIVE_DBG("hyperliquid: RAW FRAME RECEIVED len={}", payload.size());

  // parse without throwing: malformed frames from the exchange are dropped
  // instead of tearing down the websocket thread.
  const nlohmann::json root = nlohmann::json::parse(payload, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    LOG_WARN("HyperliquidDataStreamer: failed to parse message");
    LIVE_DBG("hyperliquid: PARSE FAILED: {}", payload);
    return;
  }

  const std::string channel = root.value("channel", "");
  if (channel.empty()) {
    LIVE_DBG("hyperliquid: envelope without channel, dropping frame");
    return;
  }

  // per channel, and before the dispatch: a frame that arrives and is then
  // dropped inside HandleOrderBook still has to be counted as received, or a
  // parser discarding its input is indistinguishable from a quiet venue. the
  // bare RAW FRAME line above cannot answer that -- it fires for every channel,
  // so it counts book frames and fills together.
  LIVE_DBG("hyperliquid: frame channel='{}' len={}", channel, payload.size());

  // counted before the dispatch, so a frame HandleOrderBook then rejects still
  // appears as received. Counting after the parse would make a parser
  // discarding its input look identical to a quiet venue, which is the whole
  // ambiguity these tallies exist to remove.
  if (channel == "l2Book") {
    frames_l2book_.fetch_add(1, std::memory_order_relaxed);
  } else if (channel == "trades") {
    frames_trades_.fetch_add(1, std::memory_order_relaxed);
  } else {
    frames_other_.fetch_add(1, std::memory_order_relaxed);
  }

  if (channel == "error") {
    LOG_ERROR("HyperliquidDataStreamer: server error frame: {}", payload);
    return;
  }
  // subscriptionResponse / pong / unsubscribed frames land here.
  if (channel != "l2Book" && channel != "trades") {
    LIVE_DBG("hyperliquid: ignoring channel '{}'", channel);
    return;
  }
  if (!root.contains("data")) {
    LIVE_DBG("hyperliquid: '{}' frame without data, dropping", channel);
    return;
  }

  try {
    if (channel == "l2Book") {
      // l2Book carries a single object.
      if (!root["data"].is_object()) {
        LIVE_DBG("hyperliquid: l2Book frame without data object, dropping");
        return;
      }
      HandleOrderBook(root["data"]);
    } else {
      // trades carries an array of fills.
      HandleTrades(root["data"]);
    }
  } catch (const std::exception& e) {
    LOG_WARN("HyperliquidDataStreamer: failed to handle '{}' event: {}",
             channel, e.what());
  }
}

void HyperliquidDataStreamer::HandleTrades(const nlohmann::json& data) {
  // trades payload: an array of fills.
  //   [{"coin":"BTC","side":"B","px":"65000.0","sz":"0.01",
  //     "time":<ms>,"hash":"0x...","tid":123}, ...]
  // "side" is the AGGRESSOR side: "B" lifted the ask, "A" hit the bid. that is
  // the inverse of is_buyer_maker, which asks whether the maker was the buyer
  // (i.e. the taker sold) -- same convention as BybitDataStreamer's "S" field.
  if (!data.is_array()) {
    LIVE_DBG("hyperliquid: trades data is not an array, dropping frame");
    return;
  }

  for (const auto& fill : data) {
    if (!fill.is_object()) continue;
    if (!fill.contains("px") || !fill.contains("sz") || !fill.contains("side")) {
      LIVE_DBG("hyperliquid: trade without px/sz/side, dropping");
      continue;
    }

    TradeEvent ev{};
    ev.price = ToDouble(fill["px"]);
    ev.quantity = ToDouble(fill["sz"]);
    // "time" is the exchange timestamp in ms. fall back to wall-clock
    // milliseconds so the event still carries a usable ts if it is absent.
    if (fill.contains("time") && fill["time"].is_number_integer()) {
      ev.timestamp = fill["time"].get<int64_t>();
    } else {
      ev.timestamp = NowMillis();
    }
    ev.is_buyer_maker = (fill["side"].get<std::string>() == "A");

    // per-fill console output belongs behind --debug: one line every ~180 ms at
    // 330 prints/min buries the probe report and shares an overrun_oldest sink
    // with the warnings that explain a disconnect. the LIVE_DBG line below
    // carries the same fields.
    LIVE_DBG("hyperliquid: TRADE PARSED ts={} price={} qty={} is_buyer_maker={}",
             ev.timestamp, ev.price, ev.quantity, ev.is_buyer_maker);

    EmitTrade(ev);
  }
}

void HyperliquidDataStreamer::HandleOrderBook(const nlohmann::json& data) {
  // l2Book payload:
  //   {"coin":"BTC","time":<ms>,
  //    "levels":[[{"px":"65000.0","sz":"1.5","n":2}, ...],   // bids, descending
  //              [{"px":"65001.0","sz":"0.8","n":1}, ...]]}  // asks, ascending
  // "levels" is always exactly two arrays; each level carries the price as "px"
  // and the size as "sz" (decimal strings). "n" is the order count, unused.
  // both early exits are traced: a frame that arrives and yields nothing looks
  // identical to a venue that never sent it, and the only way to tell them
  // apart is a line saying which check dropped it.
  if (!data.contains("levels") || !data["levels"].is_array()) {
    LIVE_DBG("hyperliquid: l2Book without a levels array, dropping");
    return;
  }

  const auto& levels = data["levels"];
  if (levels.size() < 2 || !levels[0].is_array() || !levels[1].is_array()) {
    LIVE_DBG("hyperliquid: l2Book levels is not [bids, asks], dropping");
    return;
  }

  const auto& bids = levels[0];
  const auto& asks = levels[1];
  if (bids.empty() || asks.empty()) {
    LIVE_DBG("hyperliquid: orderbook side empty, dropping frame");
    return;
  }

  L2Snapshot snap{};
  std::memset(&snap, 0, sizeof(L2Snapshot));

  // "time" is the exchange timestamp in ms. fall back to wall-clock
  // milliseconds so the snapshot still carries a usable ts if it is absent.
  if (data.contains("time") && data["time"].is_number_integer()) {
    snap.timestamp = data["time"].get<int64_t>();
  } else {
    snap.timestamp = NowMillis();
  }

  // hyperliquid pushes up to 20 levels per side; keep what fits the fixed
  // aggregate arrays. L2Snapshot has no per-side count field -- the untouched
  // slots simply stay zeroed.
  const size_t depth_bid = std::min<size_t>(bids.size(), MAX_L2_DEPTH);
  const size_t depth_ask = std::min<size_t>(asks.size(), MAX_L2_DEPTH);

  // written to a separate output index, not to i. a skipped level used to leave
  // a zeroed hole, and both readers of these arrays -- the validator's side scan
  // and the recorder's write loop -- stop at the first non-positive price, so
  // one malformed level silently truncated the rest of the book. compacting
  // keeps "unused slots are zeroed" true for the tail only.
  size_t out_bid = 0;
  for (size_t i = 0; i < depth_bid; ++i) {
    const auto& lvl = bids[i];
    if (!lvl.is_object() || !lvl.contains("px") || !lvl.contains("sz")) {
      LIVE_DBG("hyperliquid: malformed bid level at {}, skipping", i);
      continue;
    }
    snap.bids[out_bid].price = ToDouble(lvl["px"]);
    snap.bids[out_bid].qty   = ToDouble(lvl["sz"]);
    ++out_bid;
  }
  size_t out_ask = 0;
  for (size_t i = 0; i < depth_ask; ++i) {
    const auto& lvl = asks[i];
    if (!lvl.is_object() || !lvl.contains("px") || !lvl.contains("sz")) {
      LIVE_DBG("hyperliquid: malformed ask level at {}, skipping", i);
      continue;
    }
    snap.asks[out_ask].price = ToDouble(lvl["px"]);
    snap.asks[out_ask].qty   = ToDouble(lvl["sz"]);
    ++out_ask;
  }

  // every level on a side was malformed: compaction leaves that side empty, and
  // the best_* fields below would be read from a zeroed slot -- emitting a
  // frame with a "0.00" bid or ask. the validator rejects it downstream as
  // BookSideEmpty, but a frame already known to be bad when it is built should
  // not be emitted: in a live run that rejection is a gate failure, and the
  // gate would report a phantom venue problem while the real cause is this
  // line's absence.
  if (out_bid == 0 || out_ask == 0) {
    LIVE_DBG("hyperliquid: side had no usable levels (bids={}, asks={}), "
             "dropping frame", out_bid, out_ask);
    return;
  }

  // level 1 convenience fields, mirroring BybitDataStreamer::HandleOrderBook.
  // levels[0][0] / levels[1][0] are the best bid / best ask.
  snap.best_bid_price = snap.bids[0].price;
  snap.best_bid_qty   = snap.bids[0].qty;
  snap.best_ask_price = snap.asks[0].price;
  snap.best_ask_qty   = snap.asks[0].qty;

  // see HandleTrades: same sink, same reason.
  LIVE_DBG("hyperliquid: L2 PARSED ts={} bid={} bid_qty={} ask={} ask_qty={}",
           snap.timestamp, snap.best_bid_price, snap.best_bid_qty,
           snap.best_ask_price, snap.best_ask_qty);

  l2_emitted_.fetch_add(1, std::memory_order_relaxed);
  // a real snapshot ends the outage: the next drop is a new gap and must be
  // reported again. this is what makes the dedup above one-per-outage rather
  // than one-per-session.
  gap_reported_.store(false, std::memory_order_relaxed);
  EmitL2(snap);
}
