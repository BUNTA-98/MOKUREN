#include "binance_streamer.hpp"

#include "logger.hpp"

#include <cstring>

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

  web_socket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    switch (msg->type) {
      case ix::WebSocketMessageType::Open:
        LOG_INFO(">>> WEBSOCKET OPENED SUCCESSFULLY <<<");
        break;
      case ix::WebSocketMessageType::Close:
        LOG_WARN(">>> WEBSOCKET CLOSED (code={}, reason={})", msg->closeInfo.code, msg->closeInfo.reason);
        break;
      case ix::WebSocketMessageType::Error:
        LOG_ERROR(">>> WEBSOCKET ERROR ({}): {} <<<", msg->errorInfo.http_status, msg->errorInfo.reason);
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
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(payload);
  } catch (const std::exception& e) {
    LOG_WARN("BinanceDataStreamer: failed to parse message: {}", e.what());
    return;
  }

  // combined stream envelope: { "stream": "...", "data": { ... } }
  if (!root.contains("stream") || !root.contains("data")) {
    return;
  }

  const std::string& stream = root["stream"].get_ref<const std::string&>();
  const nlohmann::json& data = root["data"];

  try {
    if (stream.find("@aggTrade") != std::string::npos) {
      HandleAggTrade(data);
    } else if (stream.find("@bookTicker") != std::string::npos) {
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

  EmitTrade(ev);
}

void BinanceDataStreamer::HandleDepth(const nlohmann::json& data) {
  // @bookTicker payload: { "u":.., "s":"BTCUSDT", "b":"bid px", "B":"bid qty",
  //                        "a":"ask px", "A":"ask qty", "T":.., "E":.. }
  // note: "b"/"a" are the best bid/ask *prices* (strings), not arrays.
  L2Snapshot snap{};
  std::memset(&snap, 0, sizeof(L2Snapshot));

  snap.timestamp = data.at("E").get<int64_t>();

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

  EmitL2(snap);
}
