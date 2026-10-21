#include "bybit_streamer.hpp"

#include "live_debug.hpp"
#include "logger.hpp"

#include <chrono>
#include <cstring>
#include <thread>

namespace {
// bybit v5 drops connections that stay silent for ~30s, so ping well under it.
constexpr int kHeartbeatIntervalMs = 20000;
constexpr int kHeartbeatSliceMs = 100;

// a zeroed snapshot is the "book gap" sentinel: the orchestrator treats a
// frame with no bid or no ask as "the local book is no longer a faithful
// picture of the venue" and suppresses new entries until a real snapshot
// arrives. emitted on reconnect and on an unexpected close/error so the
// worker learns about the gap even though the socket recovers on its own.
L2Snapshot MakeBookGap() {
  L2Snapshot gap{};
  std::memset(&gap, 0, sizeof(L2Snapshot));
  gap.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  return gap;
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
        if (!first_open) {
          // reconnect: the local book is stale until the next snapshot.
          EmitL2(MakeBookGap());
        }
        first_open = false;
        // the subscription must be sent after the handshake completes.
        SendSubscription();
        break;
      case ix::WebSocketMessageType::Close: {
        LOG_WARN(">>> WEBSOCKET CLOSED (code={}, reason={})", msg->closeInfo.code, msg->closeInfo.reason);
        // running_ is cleared by Stop() *before* web_socket_.stop(), so a
        // close observed while it is still set was not asked for. report a
        // book gap (not an error): the socket will reconnect, and the worker
        // must stop trusting the local book until it does.
        if (running_.load(std::memory_order_acquire)) {
          EmitL2(MakeBookGap());
        }
        break;
      }
      case ix::WebSocketMessageType::Error:
        LOG_ERROR(">>> WEBSOCKET ERROR ({}): {} <<<", msg->errorInfo.http_status, msg->errorInfo.reason);
        if (running_.load(std::memory_order_acquire)) {
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
                 std::string("orderbook.1.") + symbol_};

  const std::string payload = sub.dump();
  LIVE_DBG("websocket: subscribing with {}", payload);
  web_socket_.send(payload);
}

void BybitDataStreamer::HeartbeatLoop() {
  while (keep_alive_.load(std::memory_order_acquire)) {
    // sleep in small slices so Stop() is not blocked for a full interval.
    for (int waited = 0; waited < kHeartbeatIntervalMs;
         waited += kHeartbeatSliceMs) {
      if (!keep_alive_.load(std::memory_order_acquire)) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(kHeartbeatSliceMs));
    }

    if (!keep_alive_.load(std::memory_order_acquire)) return;
    LIVE_DBG("websocket: sending heartbeat ping");
    web_socket_.send("{\"op\":\"ping\"}");
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

  // subscribe/ping acks and pong frames carry no topic; drop them quietly.
  if (!root.contains("topic") || !root.contains("data")) {
    LIVE_DBG("websocket: envelope missing topic/data keys, dropping frame");
    return;
  }

  const std::string& topic = root["topic"].get_ref<const std::string&>();
  const nlohmann::json& data = root["data"];

  LOG_DEBUG("Bybit raw stream name: {}", topic);
  LIVE_DBG("websocket: topic={}", topic);

  try {
    if (topic.rfind("publicTrade.", 0) == 0) {
      HandlePublicTrade(data);
    } else if (topic.rfind("orderbook.1.", 0) == 0) {
      HandleOrderBook(data);
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
    TradeEvent ev{};
    ev.price = std::stod(trade.at("p").get<std::string>());
    ev.quantity = std::stod(trade.at("v").get<std::string>());
    ev.timestamp = trade.at("T").get<int64_t>();
    // "S" is the taker side: "Sell" -> aggressive SELL (bid hit) -> maker buy.
    // "Buy" -> aggressive BUY (ask lift) -> maker sell.
    ev.is_buyer_maker = (trade.at("S").get<std::string>() == "Sell");

    LOG_INFO("LIVE TRADE | Price: {:.2f}, Qty: {:.4f}, IsMaker: {}", ev.price, ev.quantity, ev.is_buyer_maker);
    LIVE_DBG("websocket: TRADE PARSED ts={} price={} qty={} is_buyer_maker={}",
             ev.timestamp, ev.price, ev.quantity, ev.is_buyer_maker);

    EmitTrade(ev);
  }
}

void BybitDataStreamer::HandleOrderBook(const nlohmann::json& data) {
  // orderbook.1 payload:
  //   { "s":"BTCUSDT", "b":[["price","size"]], "a":[["price","size"]],
  //     "ts":ms, "type":"snapshot" }
  L2Snapshot snap{};
  std::memset(&snap, 0, sizeof(L2Snapshot));

  // "ts" is the matching-engine timestamp in ms. fall back to wall-clock
  // milliseconds so the snapshot still carries a usable ts if it is absent.
  if (data.contains("ts") && data["ts"].is_number_integer()) {
    snap.timestamp = data["ts"].get<int64_t>();
  } else {
    snap.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  }

  // top of book (level 1). entries are [price, size] string pairs.
  // delta frames may carry an empty side (a level being removed), so bail out
  // instead of letting at(0) throw and drop the whole frame.
  const auto& bids = data.at("b");
  const auto& asks = data.at("a");
  if (bids.empty() || asks.empty()) {
    LIVE_DBG("websocket: orderbook side empty, dropping frame");
    return;
  }

  snap.best_bid_price = std::stod(bids.at(0).at(0).get<std::string>());
  snap.best_bid_qty   = std::stod(bids.at(0).at(1).get<std::string>());
  snap.best_ask_price = std::stod(asks.at(0).at(0).get<std::string>());
  snap.best_ask_qty   = std::stod(asks.at(0).at(1).get<std::string>());

  // orderbook.1 only carries level 1; the remaining depth stays zeroed by
  // the memset above.
  snap.bids[0].price = snap.best_bid_price;
  snap.bids[0].qty   = snap.best_bid_qty;
  snap.asks[0].price = snap.best_ask_price;
  snap.asks[0].qty   = snap.best_ask_qty;

  LOG_INFO("LIVE L2   | Best Bid: {:.2f} ({:.4f}) | Best Ask: {:.2f} ({:.4f})", snap.best_bid_price, snap.best_bid_qty, snap.best_ask_price, snap.best_ask_qty);
  LIVE_DBG("websocket: L2 PARSED ts={} bid={} bid_qty={} ask={} ask_qty={}",
           snap.timestamp, snap.best_bid_price, snap.best_bid_qty,
           snap.best_ask_price, snap.best_ask_qty);

  EmitL2(snap);
}
