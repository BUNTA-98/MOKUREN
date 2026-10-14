#pragma once

#include "live_streamer.hpp"

#include <ixwebsocket/IXWebSocket.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>

// live market data source for binance spot.
// connects to the combined websocket stream (aggTrade + bookTicker) and
// forwards parsed events to the registered callbacks.
class BinanceDataStreamer : public IMarketStreamer {
public:
  // default stream: btcusdt aggTrade + bookTicker (top-of-book) on binance spot.
  explicit BinanceDataStreamer(
      const std::string& url =
          "wss://stream.binance.com:9443/stream?streams=btcusdt@aggTrade/btcusdt@bookTicker");

  ~BinanceDataStreamer() override;

  // begin streaming. non-blocking: ixwebsocket runs its own thread.
  void Start() override;

  // stop streaming and cleanly disconnect. idempotent.
  void Stop() override;

private:
  // handle a single raw websocket text frame (combined stream envelope).
  void OnMessage(const std::string& payload);

  // parse a single stream payload (already unwrapped from the envelope).
  void HandleAggTrade(const nlohmann::json& data);
  void HandleDepth(const nlohmann::json& data);

  std::string url_;
  ix::WebSocket web_socket_;
  std::atomic<bool> running_{false};
};
