#include "binance_streamer.hpp"

#include "live_debug.hpp"
#include "logger.hpp"

#include <chrono>
#include <cstring>

namespace {
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

BinanceDataStreamer::BinanceDataStreamer(const std::string& url) : url_(url) {}

BinanceDataStreamer::~BinanceDataStreamer() {
  Stop();
}

void BinanceDataStreamer::Start() {
  if (running_.exchange(true)) {
    LOG_WARN("BinanceDataStreamer::Start called while already running");
    return;
  }

  web_socket_.setUrl(url_);
  LOG_INFO("BinanceDataStreamer: setting up message callback for URL: {}", url_);

  // the socket reconnects on its own. a dropped public feed is a transient
  // transport failure, not the end of the run: ending the run on a single
  // blip would make a live session unattributable against a backtest that
  // never saw the outage.
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
        break;
      case ix::WebSocketMessageType::Close:
        LOG_WARN(">>> WEBSOCKET CLOSED (code={}, reason={})", msg->closeInfo.code, msg->closeInfo.reason);
        if (running_.load(std::memory_order_acquire)) {
          EmitL2(MakeBookGap());
        }
        break;
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

  LOG_INFO("BinanceDataStreamer: calling web_socket_.start() now...");
  web_socket_.start();
  LOG_INFO("BinanceDataStreamer: web_socket_.start() returned.");
}

void BinanceDataStreamer::Stop() {
  if (!running_.exchange(false)) {
    return;
  }

  LOG_INFO("BinanceDataStreamer: stopping websocket");
  web_socket_.stop();
}

void BinanceDataStreamer::OnMessage(const std::string& payload) {
  LIVE_DBG("websocket: RAW FRAME RECEIVED len={}", payload.size());

  nlohmann::json root;
  try {
    root = nlohmann::json::parse(payload);
  } catch (const std::exception& e) {
    LOG_WARN("BinanceDataStreamer: failed to parse message: {}", e.what());
    LIVE_DBG("websocket: PARSE FAILED: {}", e.what());
    return;
  }

  // combined stream envelope: { "stream": "...", "data": { ... } }
  if (!root.contains("stream") || !root.contains("data")) {
    LIVE_DBG("websocket: envelope missing stream/data keys, dropping frame");
    return;
  }

  const std::string& stream = root["stream"].get_ref<const std::string&>();
  const nlohmann::json& data = root["data"];

  LOG_DEBUG("Binance raw stream name: {}", stream);
  LIVE_DBG("websocket: stream={}", stream);

  try {
    if (stream.find("@aggTrade") != std::string::npos ||
        stream.find("@aggtrade") != std::string::npos) {
      HandleAggTrade(data);
    } else if (stream.find("@bookTicker") != std::string::npos ||
               stream.find("@bookticker") != std::string::npos) {
      HandleDepth(data);
    }
  } catch (const std::exception& e) {
    LOG_WARN("BinanceDataStreamer: failed to handle '{}' event: {}", stream, e.what());
  }
}

void BinanceDataStreamer::HandleAggTrade(const nlohmann::json& data) {
  TradeEvent ev{};
  ev.price = std::stod(data.at("p").get<std::string>());
  ev.quantity = std::stod(data.at("q").get<std::string>());
  ev.timestamp = data.at("T").get<int64_t>();
  // m == true  -> buyer is maker -> aggressive SELL (bid hit)
  // m == false -> aggressive BUY (ask lift)
  ev.is_buyer_maker = data.at("m").get<bool>();

  LOG_INFO("LIVE TRADE | Price: {:.2f}, Qty: {:.4f}, IsMaker: {}", ev.price, ev.quantity, ev.is_buyer_maker);
  LIVE_DBG("websocket: TRADE PARSED ts={} price={} qty={} is_buyer_maker={}",
           ev.timestamp, ev.price, ev.quantity, ev.is_buyer_maker);

  EmitTrade(ev);
}

void BinanceDataStreamer::HandleDepth(const nlohmann::json& data) {
  // @bookTicker payload: { "u":.., "s":"BTCUSDT", "b":"bid px", "B":"bid qty",
  //                        "a":"ask px", "A":"ask qty", "T":.., "E":.. }
  // note: "b"/"a" are the best bid/ask *prices* (strings), not arrays.
  L2Snapshot snap{};
  std::memset(&snap, 0, sizeof(L2Snapshot));

  // "E" (event time) is present on the futures payload but absent on spot,
  // where it would make at() throw and silently kill the L2 feed. fall back
  // to wall-clock milliseconds so the snapshot still carries a usable ts.
  if (data.contains("E") && data["E"].is_number_integer()) {
    snap.timestamp = data["E"].get<int64_t>();
  } else {
    snap.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  }

  // top of book (level 1)
  snap.best_bid_price = std::stod(data.at("b").get<std::string>());
  snap.best_bid_qty   = std::stod(data.at("B").get<std::string>());
  snap.best_ask_price = std::stod(data.at("a").get<std::string>());
  snap.best_ask_qty   = std::stod(data.at("A").get<std::string>());

  // bookTicker only carries level 1, so the depth arrays stay zeroed.
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
