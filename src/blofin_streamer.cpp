#include "blofin_streamer.hpp"

#include "data_integrity.hpp"
#include "live_debug.hpp"
#include "logger.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

// blofin drops a public socket that stays silent for ~30s, so ping well under
// it. the loop sleeps in slices so Stop() is not blocked for a full interval.
constexpr int kHeartbeatIntervalMs = 20000;
constexpr int kHeartbeatSliceMs = 100;

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// a zeroed snapshot is the "book gap" sentinel. the shape is owned by
// mokuren::integrity (MakeBookGap / IsBookGap), not by this file: the producer
// and the consumer of a sentinel must not each keep their own copy of what it
// looks like.
L2Snapshot MakeBookGap() {
  return mokuren::integrity::MakeBookGap(NowMillis());
}

// one wire number: blofin sends decimal strings on the ws pushes and numbers in
// the rest snapshot, so both are accepted. anything else reads as 0.
double JsonNumber(const nlohmann::json& v) {
  if (v.is_number()) return v.get<double>();
  if (v.is_string()) {
    try {
      return std::stod(v.get<std::string>());
    } catch (...) {
      return 0.0;
    }
  }
  return 0.0;
}

// levels are ["price","size"] pairs. malformed entries are skipped rather than
// aborting the frame: one bad level must not cost the whole book update, and a
// non-positive quantity (an erase) is a legitimate level, not junk -- dropping
// it would leave the removed level in the book forever.
void ParseLevels(const nlohmann::json& levels,
                 std::vector<mokuren::OrderBook::Level>& out) {
  out.clear();
  if (!levels.is_array()) return;
  out.reserve(levels.size());
  for (const auto& level : levels) {
    if (!level.is_array() || level.size() < 2) continue;
    const double price = JsonNumber(level.at(0));
    const double quantity = JsonNumber(level.at(1));
    if (!(price > 0.0)) continue;
    out.emplace_back(price, quantity);
  }
}

// seqId / prevSeqId arrive as strings in the docs example and as integers in
// some captures. a value that is neither reads as 0, which never equals a real
// sequence, so a malformed frame is reported as a gap instead of silently
// matching the cursor.
int64_t ParseSeq(const nlohmann::json& body, const char* key) {
  if (!body.contains(key)) return 0;
  const auto& v = body[key];
  if (v.is_number_integer()) return v.get<int64_t>();
  if (v.is_number()) return static_cast<int64_t>(v.get<double>());
  if (v.is_string()) {
    try {
      return std::stoll(v.get<std::string>());
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

}  // namespace

BloFinDataStreamer::BloFinDataStreamer(const std::string& symbol,
                                       const std::string& url)
    : symbol_(symbol), url_(url) {}

BloFinDataStreamer::~BloFinDataStreamer() {
  // joins the heartbeat thread and stops the socket before the state it
  // touches is destroyed.
  Stop();
}

void BloFinDataStreamer::Start() {
  if (running_.exchange(true)) {
    LOG_WARN("BloFinDataStreamer::Start called while already running");
    return;
  }

  web_socket_.setUrl(url_);
  LOG_INFO("BloFinDataStreamer: connecting to {}", url_);

  // a dropped public feed is a transient transport failure, not the end of the
  // run: the socket reconnects on its own and Open fires again.
  web_socket_.enableAutomaticReconnection();

  web_socket_.setOnMessageCallback(
      [this, first_open = true](const ix::WebSocketMessagePtr& msg) mutable {
        switch (msg->type) {
          case ix::WebSocketMessageType::Open:
            LOG_INFO(">>> BLOFIN WEBSOCKET OPENED <<<");
            // a fresh socket carries no book, so nothing is "lost" yet: the
            // reset cannot start the resync clock here.
            ResetOrderBook();
            connection_gap_reported_.store(false, std::memory_order_relaxed);
            if (!first_open) {
              // reconnect: the engine must stop trusting its local book until
              // the snapshot below arrives.
              EmitL2(MakeBookGap());
            }
            first_open = false;
            SendSubscription();
            break;
          case ix::WebSocketMessageType::Close: {
            const bool was_running =
                running_.load(std::memory_order_acquire);
            if (was_running) {
              LOG_WARN(">>> BLOFIN WEBSOCKET CLOSED (code={}, reason={})",
                       msg->closeInfo.code, msg->closeInfo.reason);
            } else {
              LOG_INFO(">>> BLOFIN WEBSOCKET CLOSED (code={}, reason={})",
                       msg->closeInfo.code, msg->closeInfo.reason);
            }
            ResetOrderBook();
            ReportGapOnce();
            // a reconnect the heartbeat asked for completes here, on the
            // callback thread, after the book was cleared above: this is the
            // only thread allowed to reopen the socket.
            if (reconnect_requested_.exchange(false,
                                              std::memory_order_acq_rel) &&
                was_running) {
              LOG_INFO("BloFinDataStreamer: reopening the socket after a "
                       "resync request");
              web_socket_.start();
            }
            break;
          }
          case ix::WebSocketMessageType::Error:
            LOG_ERROR(">>> BLOFIN WEBSOCKET ERROR ({}): {} <<<",
                      msg->errorInfo.http_status, msg->errorInfo.reason);
            ResetOrderBook();
            ReportGapOnce();
            break;
          case ix::WebSocketMessageType::Message:
            OnMessage(msg->str);
            break;
          default:
            break;
        }
      });

  keep_alive_.store(true, std::memory_order_release);
  heartbeat_thread_ = std::thread(&BloFinDataStreamer::HeartbeatLoop, this);
  web_socket_.start();
}

void BloFinDataStreamer::Stop() {
  if (!running_.exchange(false)) return;

  LOG_INFO("BloFinDataStreamer: stopping websocket");

  keep_alive_.store(false, std::memory_order_release);
  if (heartbeat_thread_.joinable()) {
    heartbeat_thread_.join();
  }
  web_socket_.stop();
}

void BloFinDataStreamer::SendSubscription() {
  // {"op":"subscribe","args":[{"channel":"books","instId":"BTC-USDT"}, ...]}
  //
  // the channel is "books" (200-depth, snapshot + delta), not "books50":
  // there is no books50 channel on blofin, and a subscription to a channel
  // that does not exist is accepted by the socket and then never delivers a
  // frame -- which reads as a quiet book rather than a wrong subscription.
  // "books5" exists but is snapshot-only (5 levels), so it cannot drive the
  // delta path this streamer maintains.
  nlohmann::json sub;
  sub["op"] = "subscribe";
  sub["args"] = nlohmann::json::array();
  sub["args"].push_back({{"channel", "books"}, {"instId", symbol_}});
  sub["args"].push_back({{"channel", "trades"}, {"instId", symbol_}});

  const std::string payload = sub.dump();
  LIVE_DBG("blofin: subscribing with {}", payload);
  web_socket_.send(payload);
}

void BloFinDataStreamer::RequestOrderBookSnapshot() {
  // re-subscribing books is the documented way to replay a snapshot. trades is
  // deliberately not named: that channel backfills on subscribe, so asking for
  // it again would duplicate every print since the first subscription.
  nlohmann::json sub;
  sub["op"] = "subscribe";
  sub["args"] = nlohmann::json::array();
  sub["args"].push_back({{"channel", "books"}, {"instId", symbol_}});

  const std::string payload = sub.dump();
  LIVE_DBG("blofin: requesting orderbook snapshot with {}", payload);
  web_socket_.send(payload);
}

void BloFinDataStreamer::HeartbeatLoop() {
  auto last_ping = std::chrono::steady_clock::now();
  while (keep_alive_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kHeartbeatSliceMs));
    if (!keep_alive_.load(std::memory_order_acquire)) return;

    const auto now = std::chrono::steady_clock::now();
    if (now - last_ping >= std::chrono::milliseconds(kHeartbeatIntervalMs)) {
      last_ping = now;
      LIVE_DBG("blofin: sending heartbeat ping");
      // blofin's ping is the raw string "ping", not a JSON envelope. bybit
      // wants {"op":"ping"}; sending that here is silently ignored and the
      // socket is dropped for silence ~30s later, which reads as a dead feed
      // rather than a wrong ping. the answer is the raw string "pong".
      web_socket_.send("ping");
    }

    if (!running_.load(std::memory_order_acquire)) continue;
    if (!resync_.ShouldReconnect(NowMillis())) continue;

    resync_.OnReconnectAttempted(NowMillis());

    // first expiry: re-subscribing books replays a snapshot and costs the
    // socket nothing. if that already happened for this outage, the duplicate
    // subscription was not enough -- a new connection is the only thing that
    // guarantees a snapshot.
    if (!snapshot_requested_.exchange(true, std::memory_order_acq_rel)) {
      LOG_WARN("BloFinDataStreamer: order book still unusable; re-subscribing "
               "books to force a fresh snapshot");
      RequestOrderBookSnapshot();
      continue;
    }

    LOG_WARN("BloFinDataStreamer: order book still unusable after a "
             "re-subscribe; forcing a reconnect");
    connection_gap_reported_.store(true, std::memory_order_relaxed);
    reconnect_requested_.store(true, std::memory_order_release);
    web_socket_.stop();
  }
}

void BloFinDataStreamer::OnMessage(const std::string& payload) {
  LIVE_DBG("blofin: RAW FRAME RECEIVED len={}", payload.size());

  // pong is a raw string, not JSON.
  if (payload == "pong") return;

  const nlohmann::json root = nlohmann::json::parse(payload, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    LOG_WARN("BloFinDataStreamer: dropped unparsable frame (len={})",
             payload.size());
    return;
  }

  // control frames carry "event" ("subscribe", "error", "pong") and no "arg".
  // a REJECTION has the same shape as an ack, so it is named explicitly here:
  // a refused channel and a quiet feed would otherwise be indistinguishable.
  if (root.contains("event")) {
    const std::string event = root.value("event", "");
    if (event == "error") {
      LOG_ERROR("BloFinDataStreamer: request REJECTED: {}", root.dump());
    } else if (event == "subscribe") {
      LOG_DEBUG("BloFinDataStreamer: subscribe ack: {}", root.dump());
    } else {
      LIVE_DBG("blofin: event={} {}", event, root.dump());
    }
    return;
  }

  // data frames carry "arg" (channel + instId) and "data".
  if (!root.contains("arg") || !root.contains("data")) {
    LOG_DEBUG("BloFinDataStreamer: envelope without arg/data, dropping frame");
    return;
  }

  const std::string channel = root["arg"].value("channel", "");

  try {
    if (channel == "trades") {
      HandleTrades(root["data"]);
    } else if (channel == "books") {
      // the WHOLE envelope: blofin puts "action" a level up, next to "data",
      // not inside it. reading it inside data[0] drops every frame before a
      // single level is parsed.
      HandleOrderBook(root);
    }
  } catch (const std::exception& e) {
    LOG_WARN("BloFinDataStreamer: failed to handle '{}' frame: {}", channel,
             e.what());
  }
}

void BloFinDataStreamer::HandleTrades(const nlohmann::json& data) {
  // "data" is an array of trades:
  //   { "instId":"BTC-USDT", "tradeId":"...", "price":"...", "size":"...",
  //     "side":"buy"|"sell", "ts":"<ms>" }
  if (!data.is_array()) {
    LOG_DEBUG("BloFinDataStreamer: trades payload is not an array");
    return;
  }

  for (const auto& trade : data) {
    if (!trade.is_object()) continue;
    if (!trade.contains("price") || !trade.contains("size")) continue;

    TradeEvent ev{};
    ev.price = JsonNumber(trade["price"]);
    ev.quantity = JsonNumber(trade["size"]);
    ev.timestamp = ParseSeq(trade, "ts");
    // "side" is the TAKER side: "sell" -> aggressive sell into the bid, so the
    // resting order was a buy and the maker is a buyer.
    ev.is_buyer_maker = (trade.value("side", "") == "sell");

    LIVE_DBG("blofin: TRADE PARSED ts={} price={} qty={} is_buyer_maker={}",
             ev.timestamp, ev.price, ev.quantity, ev.is_buyer_maker);
    EmitTrade(ev);
  }
}

void BloFinDataStreamer::ResetOrderBook() {
  // only a book that existed and is now gone is a loss worth resyncing. a
  // fresh connection has lost nothing: it has not been given a book yet.
  const bool had_book = book_.has_snapshot();
  book_.Clear();
  last_seq_id_ = 0;
  apply_counter_ = 0;
  snapshot_requested_.store(false, std::memory_order_relaxed);
  if (had_book && running_.load(std::memory_order_acquire)) {
    resync_.OnBookInvalidated(NowMillis());
  }
}

void BloFinDataStreamer::ReportGapOnce() {
  if (!running_.load(std::memory_order_acquire)) return;
  if (connection_gap_reported_.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  // the engine must stop trusting its local book until a real snapshot
  // arrives; the sentinel is how it learns that.
  EmitL2(MakeBookGap());
}

void BloFinDataStreamer::EmitOrderBook(int64_t ts_ms) {
  // a one-sided book is a resync in progress, not a market state. publishing
  // nothing lets the engine keep its last good book, the same way it does
  // across a gap.
  if (book_.empty()) return;

  const L2Snapshot snap = book_.ToSnapshot(ts_ms);

  // once a second is enough to see the feed is alive and roughly where the
  // book is. measured on a steady clock, not on the venue's "ts": a delta
  // carrying a slightly older timestamp would make the subtraction negative
  // and silence the heartbeat until the venue clock caught up.
  static auto last_info =
      std::chrono::steady_clock::now() - std::chrono::seconds(2);
  const auto now = std::chrono::steady_clock::now();
  if (now - last_info >= std::chrono::seconds(1)) {
    last_info = now;
    LOG_INFO(
        "LIVE L2   | Best Bid: {:.2f} ({:.4f}) | Best Ask: {:.2f} ({:.4f})",
        snap.best_bid_price, snap.best_bid_qty, snap.best_ask_price,
        snap.best_ask_qty);
  }
  LIVE_DBG("blofin: L2 ts={} bid={} bid_qty={} ask={} ask_qty={}",
           snap.timestamp, snap.best_bid_price, snap.best_bid_qty,
           snap.best_ask_price, snap.best_ask_qty);

  EmitL2(snap);
}

void BloFinDataStreamer::HandleOrderBook(const nlohmann::json& envelope) {
  // { "arg": {"channel":"books","instId":"BTC-USDT"},
  //   "action": "snapshot" | "update",            <- payload level
  //   "data": [ { "bids": [["price","size"], ...],
  //               "asks": [["price","size"], ...],
  //               "ts":   "<ms>",
  //               "seqId": "<id>", "prevSeqId": "<id>" } ] }
  const std::string action = envelope.value("action", "");
  if (action != "snapshot" && action != "update") {
    LOG_DEBUG("BloFinDataStreamer: unknown orderbook action '{}', dropping",
              action);
    return;
  }

  if (!envelope.contains("data")) {
    LOG_DEBUG("BloFinDataStreamer: orderbook frame without data, dropping");
    return;
  }
  // Docs: `data` is an OBJECT (snapshot and update both). Tolerate a
  // single-element array defensively so an exchange-side wrap can't silently
  // starve the book.
  static const nlohmann::json kEmpty = nlohmann::json::object();
  const nlohmann::json& body =
      envelope["data"].is_array()
          ? (envelope["data"].empty() ? kEmpty : envelope["data"][0])
          : envelope["data"];
  if (!body.is_object()) {
    LOG_DEBUG("BloFinDataStreamer: orderbook frame data is not an object, "
              "dropping");
    return;
  }

  std::vector<mokuren::OrderBook::Level> bids;
  std::vector<mokuren::OrderBook::Level> asks;
  if (body.contains("bids")) ParseLevels(body["bids"], bids);
  if (body.contains("asks")) ParseLevels(body["asks"], asks);

  int64_t ts_ms = ParseSeq(body, "ts");
  if (ts_ms <= 0) {
    // keep the local clock; a bad ts must not cost the frame.
    ts_ms = NowMillis();
  }

  const int64_t seq_id = ParseSeq(body, "seqId");

  if (action == "snapshot") {
    // replace outright: the previous state is superseded, so nothing is
    // invalidated and the watchdog is only cleared below if the result is
    // usable.
    book_.Clear();
    last_seq_id_ = seq_id;
    // the venue's ids are non-contiguous, so OrderBook gets a local counter
    // that never gaps: its "+1" rule exists to catch a missed frame, and the
    // prevSeqId check below is what actually catches it here.
    book_.ApplySnapshot(bids, asks, ++apply_counter_);
    if (!book_.empty()) {
      resync_.OnSnapshotApplied();
      snapshot_requested_.store(false, std::memory_order_relaxed);
      EmitOrderBook(ts_ms);
    } else {
      LOG_DEBUG("BloFinDataStreamer: snapshot left a one-sided book; waiting "
                "for the next one");
    }
    return;
  }

  // action == "update"
  if (!book_.has_snapshot()) {
    // a delta applied to nothing makes a book that is half real and half
    // invented.
    LOG_DEBUG("BloFinDataStreamer: dropped book delta before snapshot");
    return;
  }

  const int64_t prev = ParseSeq(body, "prevSeqId");
  if (prev != last_seq_id_) {
    // a missed update leaves a removed level present or a grown level small,
    // and nothing downstream can detect it: the frame that results is
    // structurally valid. resync instead of trading on it.
    LOG_WARN("BloFinDataStreamer: book gap prevSeqId={} expected seqId={}; "
             "resyncing from a fresh snapshot",
             prev, last_seq_id_);
    ResetOrderBook();
    EmitL2(MakeBookGap());
    RequestOrderBookSnapshot();
    return;
  }

  last_seq_id_ = seq_id;
  book_.ApplyDelta(bids, asks, ++apply_counter_);
  EmitOrderBook(ts_ms);
}

bool BloFinDataStreamer::HandleFrameForTests(const std::string& payload) {
  // runs the same parser the socket uses, so a test that passes here is a test
  // about the wire format, not about a parallel implementation.
  if (payload == "pong") return false;

  const nlohmann::json j = nlohmann::json::parse(payload, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return false;
  if (j.contains("event")) return false;
  if (!j.contains("arg") || !j.contains("data")) return false;

  const std::string channel = j["arg"].value("channel", "");
  if (channel != "trades" && channel != "books") return false;

  OnMessage(payload);
  return true;
}
