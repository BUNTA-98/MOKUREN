#pragma once

#include "live_streamer.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <string>

// Live market-data adapter for the Hyperliquid public websocket API.
//
// Owns a single ix::WebSocket connection to wss://api.hyperliquid.xyz/ws and
// subscribes to the public l2Book AND trades feeds as soon as the socket
// opens. Book frames become L2Snapshots (SetL2Callback), fills become
// TradeEvents (SetTradeCallback).
//
// register BOTH callbacks before calling Start(): the bar aggregator only
// advances on trades, so a session that received nothing but book frames never
// closes a candle and never evaluates a trigger.
class HyperliquidDataStreamer : public IMarketStreamer {
public:
  // default stream: hyperliquid public endpoint for BTC.
  explicit HyperliquidDataStreamer(
      const std::string& symbol = "BTC",
      const std::string& url = "wss://api.hyperliquid.xyz/ws");

  ~HyperliquidDataStreamer() override;

  HyperliquidDataStreamer(const HyperliquidDataStreamer&) = delete;
  HyperliquidDataStreamer& operator=(const HyperliquidDataStreamer&) = delete;

  // begin streaming. non-blocking: ixwebsocket runs its own thread.
  void Start() override;

  // stop streaming and join the websocket thread. idempotent.
  void Stop() override;

  bool IsConnected() const { return connected_.load(); }

  // ---- Testing Hooks (Public for Catch2) ----
  // dispatches one raw websocket frame by its "channel" field.
  void HandleMessage(const std::string& payload);
  // maps the "data" object of one l2Book push onto an L2Snapshot.
  void HandleOrderBook(const nlohmann::json& data);
  // maps the "data" array of one trades push onto TradeEvents.
  void HandleTrades(const nlohmann::json& data);

private:
  // registers the url, the reconnect policy and the message callback.
  // does not start the socket -- Start() does.
  void Connect();

  // sends the l2Book AND trades subscriptions for symbol_. trades is not
  // optional: the bar aggregator only advances on TradeEvents, so an
  // l2Book-only session closes no candle.
  void SendSubscription();

  std::string symbol_;
  std::string url_;

  ix::WebSocket ws_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};

  // per-channel tallies, summarised once in Stop(). The recorder's own stats
  // start one layer later, at EmitL2, so they cannot distinguish "the venue
  // pushes books this rarely" from "HandleOrderBook discarded the frame" --
  // the only question a thin l2 channel raises. Atomic because they are
  // written on the ixwebsocket callback thread and read on the caller's.
  std::atomic<uint64_t> frames_l2book_{0};
  std::atomic<uint64_t> frames_trades_{0};
  std::atomic<uint64_t> frames_other_{0};
  std::atomic<uint64_t> l2_emitted_{0};

  // one gap per outage, cleared by the first real snapshot.
  //
  // ixwebsocket reports a single failed connection three times -- Error, then
  // Close, then Open -- and all three handlers used to emit the sentinel. the
  // gate counts each one as a reconnect, so one transient drop was reported as
  // three: enough to fail Development() (max_reconnects 0) and Production()
  // (max_reconnects 1) on a feed that recovered immediately. set on the first
  // signalled gap, cleared once an actual book arrives, so the NEXT outage is
  // reported again.
  std::atomic<bool> gap_reported_{false};
};
