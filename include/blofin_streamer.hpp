#pragma once

#include "live_streamer.hpp"
#include "order_book.hpp"
#include "resync_watchdog.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

// BloFin public market-data feed: `books` (200-depth snapshot + delta) and
// `trades`.
//
// SEQUENCING LIVES HERE, NOT IN OrderBook. BloFin's seqIds are non-contiguous
// on the wire (the docs' own example steps 107600747 -> 107600806), so feeding
// them to OrderBook's "+1" rule would report GapDetected on every delta and the
// streamer would spend its life re-subscribing. The venue's prevSeqId-vs-last
// seqId comparison is the only real gap signal; OrderBook receives a strictly
// monotonic local counter that never gaps.
class BloFinDataStreamer : public IMarketStreamer {
 public:
  explicit BloFinDataStreamer(
      const std::string& symbol = "BTC-USDT",
      const std::string& url = "wss://openapi.blofin.com/ws/public");
  ~BloFinDataStreamer() override;

  void Start() override;
  void Stop() override;

  // test-only: drive the parser without a socket.
  void FeedForTesting(const std::string& payload) { OnMessage(payload); }

  // the same hook, answering whether the frame carried market data. a test
  // that asserts "this pong produced no trade" needs a return value it can
  // check; FeedForTesting cannot give one.
  bool HandleFrameForTests(const std::string& payload);

 private:
  void OnMessage(const std::string& payload);
  void HandleTrades(const nlohmann::json& data);
  // the whole envelope: blofin puts "action" a level above "data", so a handler
  // that only receives data[0] can never tell a snapshot from a delta.
  void HandleOrderBook(const nlohmann::json& envelope);
  void SendSubscription();
  void RequestOrderBookSnapshot();
  void HeartbeatLoop();
  void ResetOrderBook();
  void EmitOrderBook(int64_t ts_ms);
  // emit the integrity sentinel at most once per outage, so a close followed
  // by an error does not tell the engine twice that it lost the book.
  void ReportGapOnce();

  std::string symbol_;
  std::string url_;
  ix::WebSocket web_socket_;
  std::atomic<bool> running_{false};
  std::atomic<bool> keep_alive_{false};
  std::thread heartbeat_thread_;

  mokuren::OrderBook book_;
  mokuren::ResyncWatchdog resync_;
  std::atomic<bool> connection_gap_reported_{false};

  // set by the heartbeat thread when a re-subscribe failed to produce a
  // snapshot; consumed by the websocket callback thread, which is the only
  // thread allowed to reopen the socket.
  std::atomic<bool> reconnect_requested_{false};
  // true once the current outage was answered with a books re-subscribe, so
  // the next watchdog expiry escalates to a reconnect instead of re-asking.
  std::atomic<bool> snapshot_requested_{false};

  // venue's sequence cursor, compared against the next frame's prevSeqId.
  int64_t last_seq_id_ = 0;
  // local, contiguous id handed to OrderBook. never compared across frames.
  int64_t apply_counter_ = 0;
};
