#pragma once

#include "live_streamer.hpp"

#include <ixwebsocket/IXWebSocket.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>

// live market data source for binance usdm futures.
// connects to the combined websocket stream (aggTrade + depth) and
// forwards parsed events to the registered callbacks.
class BinanceDataStreamer : public IMarketStreamer {
public:
  // default stream: btcusdt aggTrade + depth@100ms on binance futures.
  explicit BinanceDataStreamer(
      const std::string& url =
          "wss://fstream.binance.com/stream?streams=btcusdt@aggTrade/btcusdt@depth@100ms");

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
