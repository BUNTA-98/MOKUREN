#include "bybit_streamer.hpp"

#include "data_integrity.hpp"
#include "live_debug.hpp"
#include "logger.hpp"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace {
// bybit v5 drops connections that stay silent for ~30s, so ping well under it.
constexpr int kHeartbeatIntervalMs = 20000;
constexpr int kHeartbeatSliceMs = 100;

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// a zeroed snapshot is the "book gap" sentinel: the FeedRouter treats a frame
// with no bid or no ask as "the local book is no longer a faithful picture of
// the venue" and suppresses new entries until a real snapshot arrives. emitted
// on reconnect and on an unexpected close/error so the worker learns about the
// gap even though the socket recovers on its own.
//
// the shape is owned by mokuren::integrity (IsBookGap), not by this file: the
// producer and the consumer of a sentinel must not each keep their own copy of
// what it looks like, or the day one of them grows a field the other one stops
// recognising the drop.
L2Snapshot MakeBookGap() {
  return mokuren::integrity::MakeBookGap(NowMillis());
}

// ["price","size"] decimal-string pairs -> typed levels. malformed entries are
// skipped rather than aborting the frame: one bad level must not cost the
// whole book update, and a non-positive quantity (an erase) is a legitimate
// level, not junk.
void ParseLevels(const nlohmann::json& levels,
                 std::vector<mokuren::OrderBook::Level>& out) {
  if (!levels.is_array()) return;
  out.reserve(levels.size());
  for (const auto& level : levels) {
    if (!level.is_array() || level.size() < 2) continue;
    if (!level.at(0).is_string() || !level.at(1).is_string()) continue;
    out.emplace_back(std::stod(level.at(0).get<std::string>()),
                     std::stod(level.at(1).get<std::string>()));
  }
}
}  // namespace

BybitDataStreamer::BybitDataStreamer(const std::string& symbol,
                                     const std::string& url)
    : symbol_(symbol), url_(url) {}

BybitDataStreamer::~BybitDataStreamer() {
  Stop();
}

void BybitDataStreamer::Start() {
  if (running_.exchange(true)) {
    LOG_WARN("BybitDataStreamer::Start called while already running");
    return;
  }

  web_socket_.setUrl(url_);
  LOG_INFO("BybitDataStreamer: setting up message callback for URL: {}", url_);

  // the socket reconnects on its own. a dropped public feed is a transient
  // transport failure, not the end of the run: the private state feed already
  // reconnects, and ending the run on a single blip would make a live session
  // unattributable against a backtest that never saw the outage.
  web_socket_.enableAutomaticReconnection();

  // first_open is captured by value and mutated by the callback: the first
  // Open is the initial handshake (no gap to report), every subsequent Open
  // is a reconnect and must be preceded by a book-gap frame so the worker
  // stops trusting the local book until a real snapshot arrives.
  web_socket_.setOnMessageCallback(
      [this, first_open = true](const ix::WebSocketMessagePtr& msg) mutable {
    switch (msg->type) {
      case ix::WebSocketMessageType::Open:
        LOG_INFO(">>> WEBSOCKET OPENED SUCCESSFULLY <<<");
        // a new socket has no book. the local maps belong to the previous
        // connection, and the first snapshot on this one will rebuild them.
        ResetOrderBook();
        // this connection is live again, so it owes no further gap report.
        connection_gap_reported_.store(false, std::memory_order_relaxed);
        if (!first_open) {
          // reconnect: the local book is stale until the next snapshot.
          EmitL2(MakeBookGap());
        }
        first_open = false;
        // the subscription must be sent after the handshake completes.
        SendSubscription();
        break;
      case ix::WebSocketMessageType::Close: {
        // a close we asked for is not a fault: Stop() clears running_ before
        // stopping the socket, so the flag still being set here is what
        // distinguishes a dropped feed from a clean shutdown. logging both at
        // warning level made every normal exit look like something went wrong.
        if (running_.load(std::memory_order_acquire)) {
          LOG_WARN(">>> WEBSOCKET CLOSED (code={}, reason={})",
                   msg->closeInfo.code, msg->closeInfo.reason);
        } else {
          LOG_INFO(">>> WEBSOCKET CLOSED (code={}, reason={})",
                   msg->closeInfo.code, msg->closeInfo.reason);
        }
        // running_ is cleared by Stop() *before* web_socket_.stop(), so a
        // close observed while it is still set was not asked for. report a
        // book gap (not an error): the socket will reconnect, and the worker
        // must stop trusting the local book until it does.
        // the book is stale whether or not we are shutting down, so it is
        // dropped unconditionally; only the sentinel is conditional. Error and
        // Close both land here for one failed connection, and the gate counts
        // every sentinel as a reconnect, so the report is latched.
        ResetOrderBook();
        if (running_.load(std::memory_order_acquire) &&
            !connection_gap_reported_.load(std::memory_order_relaxed)) {
          connection_gap_reported_.store(true, std::memory_order_relaxed);
          EmitL2(MakeBookGap());
        }

        // a reconnect the heartbeat asked for completes here, on the callback
        // thread, after the book was cleared above. starting it from the
        // heartbeat thread instead let a late Close run ResetOrderBook() after
        // the new connection's snapshot had landed -- wiping a good book and
        // leaving l2 silent until the next one.
        if (reconnect_requested_.exchange(false, std::memory_order_acq_rel) &&
            running_.load(std::memory_order_acquire)) {
          LOG_INFO("BybitDataStreamer: reopening the socket after a resync "
                   "request");
          web_socket_.start();
        }
        break;
      }
      case ix::WebSocketMessageType::Error:
        LOG_ERROR(">>> WEBSOCKET ERROR ({}): {} <<<", msg->errorInfo.http_status, msg->errorInfo.reason);
        // the Error that precedes a Close: report the gap here, and let the
        // Close that follows see the latch and stay quiet.
        ResetOrderBook();
        if (running_.load(std::memory_order_acquire) &&
            !connection_gap_reported_.load(std::memory_order_relaxed)) {
          connection_gap_reported_.store(true, std::memory_order_relaxed);
          EmitL2(MakeBookGap());
        }
        break;
      case ix::WebSocketMessageType::Message:
        OnMessage(msg->str);
        break;
      default:
        break;
    }
  });

  // start the heartbeat alongside the socket so the thread is alive as soon
  // as the subscription is acknowledged.
  keep_alive_.store(true, std::memory_order_release);
  heartbeat_thread_ = std::thread(&BybitDataStreamer::HeartbeatLoop, this);

  LOG_INFO("BybitDataStreamer: calling web_socket_.start() now...");
  web_socket_.start();
  LOG_INFO("BybitDataStreamer: web_socket_.start() returned.");
}

void BybitDataStreamer::Stop() {
  if (!running_.exchange(false)) {
    return;
  }

  LOG_INFO("BybitDataStreamer: stopping websocket");

  keep_alive_.store(false, std::memory_order_release);
  if (heartbeat_thread_.joinable()) {
    heartbeat_thread_.join();
  }

  web_socket_.stop();
}

void BybitDataStreamer::SendSubscription() {
  nlohmann::json sub;
  sub["op"] = "subscribe";
  sub["args"] = {std::string("publicTrade.") + symbol_,
                 std::string("orderbook.50.") + symbol_};

  const std::string payload = sub.dump();
  LIVE_DBG("websocket: subscribing with {}", payload);
  web_socket_.send(payload);
}

void BybitDataStreamer::HeartbeatLoop() {
  auto last_ping = std::chrono::steady_clock::now();
  while (keep_alive_.load(std::memory_order_acquire)) {
    // sleep in small slices so Stop() is not blocked for a full interval, and
    // so the resync watchdog is checked on a 100ms cadence rather than the 20s
    // ping cadence: a resync that waits out a heartbeat interval is
    // indistinguishable from a dead feed.
    std::this_thread::sleep_for(std::chrono::milliseconds(kHeartbeatSliceMs));
    if (!keep_alive_.load(std::memory_order_acquire)) return;

    const auto now = std::chrono::steady_clock::now();
    if (now - last_ping >= std::chrono::milliseconds(kHeartbeatIntervalMs)) {
      last_ping = now;
      LIVE_DBG("websocket: sending heartbeat ping");
      web_socket_.send("{\"op\":\"ping\"}");
    }

    if (!running_.load(std::memory_order_acquire)) continue;
    if (!resync_.ShouldReconnect(NowMillis())) continue;

    LOG_WARN("BybitDataStreamer: order book still unusable after the resync "
             "grace period; forcing a reconnect (a new socket always replays "
             "a snapshot, a duplicate subscription is not guaranteed to)");
    resync_.OnReconnectAttempted(NowMillis());
    // the loss was already reported where the book was invalidated, so the
    // Close that follows must not report it a second time. Open clears this
    // and reports the reconnect itself.
    connection_gap_reported_.store(true, std::memory_order_relaxed);

    // request only. the Close handler runs on the callback thread and owns
    // book_, so the new socket is opened there -- after teardown, never
    // concurrently with it. if no Close ever arrives (the socket was already
    // dead) stop() is a no-op, the flag stays set, and OnReconnectAttempted
    // above has widened the backoff so this retries at a sane cadence.
    reconnect_requested_.store(true, std::memory_order_release);
    web_socket_.stop();
  }
}

void BybitDataStreamer::OnMessage(const std::string& payload) {
  LIVE_DBG("websocket: RAW FRAME RECEIVED len={}", payload.size());

  nlohmann::json root;
  try {
    root = nlohmann::json::parse(payload);
  } catch (const std::exception& e) {
    LOG_WARN("BybitDataStreamer: failed to parse message: {}", e.what());
    LIVE_DBG("websocket: PARSE FAILED: {}", e.what());
    return;
  }

  // bybit answers a subscribe with {"success":true|false,"ret_msg":...} and no
  // "topic". a REJECTION looks the same shape, so the guard below discarded it
  // as an ack: a refused topic and a quiet feed were indistinguishable, which
  // is exactly what made the l2=0 investigation ambiguous.
  if (root.contains("success") && root["success"].is_boolean()) {
    if (!root["success"].get<bool>()) {
      LOG_ERROR("BybitDataStreamer: request REJECTED: {}", root.dump());
    } else {
      LOG_DEBUG("BybitDataStreamer: {} ack: {}", root.value("op", ""),
                root.value("ret_msg", ""));
    }
    return;
  }

  // subscribe/ping acks and pong frames carry no topic; drop them quietly.
  if (!root.contains("topic") || !root.contains("data")) {
    LIVE_DBG("websocket: envelope missing topic/data keys, dropping frame");
    return;
  }

  const std::string& topic = root["topic"].get_ref<const std::string&>();
  const nlohmann::json& data = root["data"];

  // one topic line, not two: the LOG_DEBUG that used to sit here said the same
  // thing as the LIVE_DBG below and fired once per orderbook frame.
  LIVE_DBG("websocket: topic={}", topic);

  try {
    if (topic.rfind("publicTrade.", 0) == 0) {
      HandlePublicTrade(data);
    } else if (topic.rfind("orderbook.50.", 0) == 0) {
      // the ENVELOPE, not "data": bybit puts "type" and "ts" a level up.
      // passing "data" made HandleOrderBook's first guard fail on every frame.
      HandleOrderBook(root);
    }
  } catch (const std::exception& e) {
    LOG_WARN("BybitDataStreamer: failed to handle '{}' event: {}", topic, e.what());
  }
}

void BybitDataStreamer::HandlePublicTrade(const nlohmann::json& data) {
  // "data" is an array of trades:
  //   { "T":ms, "s":"BTCUSDT", "S":"Buy"/"Sell", "v":"qty", "p":"price", ... }
  if (!data.is_array()) {
    LIVE_DBG("websocket: publicTrade data is not an array, dropping frame");
    return;
  }

  for (const auto& trade : data) {
    // one malformed entry must not cost the whole batch. .at() on a missing
    // field throws, and letting that reach OnMessage's catch dropped every
    // trade after it in the same array -- one bad print silently losing the
    // nineteen good ones behind it.
    TradeEvent ev{};
    try {
      ev.price = std::stod(trade.at("p").get<std::string>());
      ev.quantity = std::stod(trade.at("v").get<std::string>());
      ev.timestamp = trade.at("T").get<int64_t>();
      // "S" is the taker side: "Sell" -> aggressive SELL (bid hit) -> maker
      // buy. "Buy" -> aggressive BUY (ask lift) -> maker sell.
      ev.is_buyer_maker = (trade.at("S").get<std::string>() == "Sell");
    } catch (const std::exception& e) {
      LOG_WARN("BybitDataStreamer: skipping malformed trade entry: {}",
               e.what());
      continue;
    }

    // no INFO per raw trade: on a liquid pair that is tens of lines a second,
    // and it buried the one line that mattered. the validator counts every
    // trade it accepts and the book heartbeat below shows the feed is alive;
    // what a single venue print does not deserve is a log line of its own.
    LIVE_DBG("websocket: TRADE PARSED ts={} price={} qty={} is_buyer_maker={}",
             ev.timestamp, ev.price, ev.quantity, ev.is_buyer_maker);

    EmitTrade(ev);
  }
}

void BybitDataStreamer::ResetOrderBook() {
  // a fresh connection has lost nothing: it has not been given a book yet.
  // arming the watchdog unconditionally made every Open start a 3s countdown,
  // and with max_reconnects=0 that turned a healthy start into "feed
  // reconnected 1 time(s)" 3.1s in, before the first snapshot could arrive.
  // only a book that existed and is now gone is a loss worth resyncing.
  const bool had_book = book_.has_snapshot();
  book_.Clear();
  // before the engine is armed there is nothing to resync either: the startup
  // gate decides whether a feed that never produced a book is fit to run.
  if (had_book && running_.load(std::memory_order_acquire)) {
    resync_.OnBookInvalidated(NowMillis());
  }
}

void BybitDataStreamer::RequestOrderBookSnapshot() {
  nlohmann::json sub;
  sub["op"] = "subscribe";
  // only the book topic: re-subscribing publicTrade would replay trades that
  // are already aggregated into the current candle.
  sub["args"] = {std::string("orderbook.50.") + symbol_};
  const std::string payload = sub.dump();
  LIVE_DBG("websocket: requesting orderbook snapshot with {}", payload);
  web_socket_.send(payload);
}

void BybitDataStreamer::EmitOrderBook(int64_t ts_ms) {
  // a one-sided book is a resync in progress, not a market state. publishing
  // nothing lets the engine keep its last good book, the same way it does
  // across a gap; publishing a half-book would be rejected as BookSideEmpty,
  // which is fatal, and would end a live run over a transient.
  if (book_.empty()) return;

  const L2Snapshot snap = book_.ToSnapshot(ts_ms);

  // per-frame INFO is a flood, not a signal: orderbook.50 pushes tens of
  // frames a second, so this buried everything that mattered and grew the log
  // file without bound. once a second is enough to see the feed is alive and
  // roughly where the book is.
  //
  // measured on a steady clock, not on the venue's "ts": a delta carrying a
  // slightly older timestamp would make the subtraction negative and silence
  // the heartbeat until the venue clock caught up, which reads as a dead feed.
  static auto last_info = std::chrono::steady_clock::now() -
                          std::chrono::seconds(2);
  const auto now = std::chrono::steady_clock::now();
  if (now - last_info >= std::chrono::seconds(1)) {
    last_info = now;
    LOG_INFO("LIVE L2   | Best Bid: {:.2f} ({:.4f}) | Best Ask: {:.2f} ({:.4f})",
             snap.best_bid_price, snap.best_bid_qty, snap.best_ask_price,
             snap.best_ask_qty);
  }
  LIVE_DBG("websocket: L2 PARSED ts={} bid={} bid_qty={} ask={} ask_qty={}",
           snap.timestamp, snap.best_bid_price, snap.best_bid_qty,
           snap.best_ask_price, snap.best_ask_qty);

  EmitL2(snap);
}

void BybitDataStreamer::HandleOrderBook(const nlohmann::json& envelope) {
  // bybit v5 orderbook.50 message:
  //   { "topic": "orderbook.50.BTCUSDT",
  //     "type":  "snapshot" | "delta",              <- envelope level
  //     "ts":    <matching-engine ms>,              <- envelope level
  //     "data":  { "s": "BTCUSDT",
  //                "b": [["price","size"], ...],
  //                "a": [["price","size"], ...],
  //                "u": <update id>, "seq": <cross seq> } }
  //
  // "type" is NOT inside "data". reading it there is why every book frame was
  // dropped by the guard below before a single level was parsed: l2 delivered
  // zero frames and reported zero rejections, because the frame never reached
  // the parser. "ts" was misread the same way and silently fell back to the
  // local clock, which made every frame look fresh to the staleness check.
  //
  // the parameter is the envelope. a bare "data" object is still detected and
  // read for levels, but it carries no "type", so it is dropped rather than
  // guessed at: a delta forwarded as a snapshot is a book that silently lost
  // every level the delta did not mention.
  //
  // the drop reasons below are LOG_DEBUG, not LIVE_DBG: LIVE_DBG is gated on
  // g_live_debug_enabled, which is off by default, so those messages were
  // invisible during exactly the investigation they existed for.
  const nlohmann::json* levels_ptr = &envelope;
  if (envelope.contains("data") && envelope["data"].is_object()) {
    levels_ptr = &envelope["data"];
  }
  const nlohmann::json& levels = *levels_ptr;

  if (!envelope.contains("type") || !envelope["type"].is_string()) {
    LOG_DEBUG("BybitDataStreamer: orderbook frame without envelope type, "
              "dropping (no levels applied)");
    return;
  }
  const std::string type = envelope["type"].get<std::string>();

  int64_t ts_ms = NowMillis();
  if (envelope.contains("ts") && envelope["ts"].is_number_integer()) {
    ts_ms = envelope["ts"].get<int64_t>();
  }

  const bool has_u = levels.contains("u") && levels["u"].is_number_integer();
  const int64_t update_id = has_u ? levels["u"].get<int64_t>() : 0;

  std::vector<mokuren::OrderBook::Level> bids;
  std::vector<mokuren::OrderBook::Level> asks;
  if (levels.contains("b")) ParseLevels(levels.at("b"), bids);
  if (levels.contains("a")) ParseLevels(levels.at("a"), asks);

  mokuren::OrderBook::ApplyResult result =
      mokuren::OrderBook::ApplyResult::Ignored;
  if (type == "snapshot") {
    result = book_.ApplySnapshot(bids, asks, update_id);
  } else if (type == "delta") {
    result = book_.ApplyDelta(bids, asks, update_id);
  } else {
    LOG_DEBUG("BybitDataStreamer: unknown orderbook type '{}', dropping", type);
    return;
  }

  switch (result) {
    case mokuren::OrderBook::ApplyResult::Ignored:
      // a delta with no snapshot behind it. before the engine is armed the
      // startup gate owns the decision; while running, the resync clock was
      // started by whichever reset dropped the book, so there is nothing to do
      // here but wait for the replay.
      LOG_DEBUG("BybitDataStreamer: orderbook {} ignored (no snapshot yet)",
                type);
      return;
    case mokuren::OrderBook::ApplyResult::GapDetected:
      // a missed delta leaves a removed level present or a grown level small,
      // and nothing downstream can detect it: the frame that results is
      // structurally valid. resync instead of trading on it.
      LOG_WARN("BybitDataStreamer: orderbook update gap (u={}); resyncing "
               "from a fresh snapshot",
               update_id);
      ResetOrderBook();
      EmitL2(MakeBookGap());
      RequestOrderBookSnapshot();
      return;
    case mokuren::OrderBook::ApplyResult::Applied:
      break;
  }

  // only a snapshot that left both sides populated ends the wait. a one-sided
  // snapshot means the venue is still filling the book, and telling the
  // watchdog otherwise would leave it waiting forever.
  if (type == "snapshot" && !book_.empty()) {
    resync_.OnSnapshotApplied();
  }

  EmitOrderBook(ts_ms);
}
