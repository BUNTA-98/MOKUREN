#pragma once

#include "live_streamer.hpp"
#include "order_book.hpp"
#include "resync_watchdog.hpp"

#include <ixwebsocket/IXWebSocket.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>
#include <thread>

// live market data source for bybit v5 (public linear).
// connects to the public websocket, subscribes to publicTrade + orderbook.50
// for a single symbol, maintains the local book from the snapshot+delta stream,
// and forwards parsed events to the registered callbacks.
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

  // send the v5 subscription request for publicTrade and orderbook.50.
  void SendSubscription();

  // periodic {"op":"ping"} so the exchange does not drop an idle connection.
  void HeartbeatLoop();

  // forget the local book. called on every socket open/close/error and on a
  // detected update gap: whatever was accumulated belongs to a connection (or
  // a point in the sequence) that no longer holds.
  void ResetOrderBook();

  // ask the venue to replay the book. subscribing again is the documented way
  // to get a fresh snapshot; publicTrade is deliberately not named, or the
  // trade stream would be duplicated.
  void RequestOrderBookSnapshot();

  // publish the top of the local book. no-op while either side is empty: a
  // one-sided book is a resync in progress, and the validator marks it fatal,
  // which would end a live run over a transient.
  void EmitOrderBook(int64_t ts_ms);

  std::string symbol_;
  std::string url_;
  ix::WebSocket web_socket_;
  std::atomic<bool> running_{false};

  std::atomic<bool> keep_alive_{false};
  std::thread heartbeat_thread_;

  // local book, maintained by applying orderbook.50 snapshots and deltas.
  //
  // a delta carries only the levels that changed, so forwarding one as a
  // snapshot hands the engine a book that silently lost every level the delta
  // did not mention -- structurally valid, deeply wrong, and invisible to the
  // depth check. that is why this state exists and why the subscription is
  // depth 50 rather than depth 1.
  //
  // the apply rules live in OrderBook so they can be tested without a socket:
  // a wrong book produces frames that pass every validator rule, so nothing
  // downstream would notice. only touched from the ixwebsocket callback thread
  // (message/open/close/error all arrive there), so it needs no lock.
  mokuren::OrderBook book_;

  // re-subscribing to an already-subscribed topic is documented to replay a
  // snapshot, but it is not something a new connection can promise -- whereas
  // a new connection always sends one, because that is the handshake the first
  // snapshot already relies on. this watches how long the book has been
  // unusable and escalates to a reconnect, so a missed delta cannot leave l2
  // silent for the rest of the session.
  mokuren::ResyncWatchdog resync_;

  // one gap per disconnection. ixwebsocket fires Error and then Close for the
  // same failed connection, and both handlers emit a sentinel: without this,
  // a single dropped socket was counted as two reconnects by the startup gate,
  // which is enough to fail a probe that allows zero. cleared on every Open.
  //
  // atomic because the heartbeat thread sets it before forcing a reconnect and
  // the callback thread reads it on Close.
  std::atomic<bool> connection_gap_reported_{false};
};
