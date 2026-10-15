#pragma once

#include "live_streamer.hpp"

#include <ixwebsocket/IXWebSocket.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>
#include <thread>

// live market data source for bybit v5 (public linear).
// connects to the public websocket, subscribes to publicTrade + orderbook.1
// for a single symbol, and forwards parsed events to the registered callbacks.
class BybitDataStreamer : public IMarketStreamer {
public:
  // default stream: bybit v5 public linear endpoint for btcusdt.
  explicit BybitDataStreamer(
      const std::string& symbol = "BTCUSDT",
      const std::string& url = "wss://stream.bybit.com/v5/public/linear");

  ~BybitDataStreamer() override;

  // begin streaming. non-blocking: ixwebsocket runs its own thread.
  void Start() override;

  // stop streaming and cleanly disconnect. idempotent.
  void Stop() override;

private:
  // handle a single raw websocket text frame (v5 envelope).
  void OnMessage(const std::string& payload);

  // parse a single stream payload (already unwrapped from the envelope).
  void HandlePublicTrade(const nlohmann::json& data);
  void HandleOrderBook(const nlohmann::json& data);

  // send the v5 subscription request for publicTrade and orderbook.1.
  void SendSubscription();

  // periodic {"op":"ping"} so the exchange does not drop an idle connection.
  void HeartbeatLoop();

  std::string symbol_;
  std::string url_;
  ix::WebSocket web_socket_;
  std::atomic<bool> running_{false};

  std::atomic<bool> keep_alive_{false};
  std::thread heartbeat_thread_;
};
