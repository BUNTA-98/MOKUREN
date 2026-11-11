#pragma once

#include "live_streamer.hpp"
#include "order_book.hpp"
#include "resync_watchdog.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
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

  // What arrived on the wire. A session that wrote no usable frame has to be
  // able to say WHY, and the recorder's counters cannot: they start one layer
  // later, at EmitL2, so "the venue sent nothing", "the venue refused the
  // subscription" (see last_rejection) and "every frame was dropped by the
  // parser" all read as l2_written = 0.
  struct WireStats {
    // upgrades that completed. 0 means the refusal happened at the handshake,
    // before any frame could arrive -- a different problem from "connected and
    // then sent nothing", and the two used to print an identical summary.
    uint64_t opens = 0;
    uint64_t books = 0;           // books envelopes received
    uint64_t trades = 0;          // trades envelopes received
    uint64_t control = 0;         // acks, error replies, pongs
    uint64_t unparsable = 0;      // frames that were not JSON objects
    // real book frames handed to the engine; the gap sentinel is not counted
    // here -- the recorder counts those as l2_gaps, and counting it twice
    // would make a reconnect look like market data.
    uint64_t l2_emitted = 0;
    uint64_t trades_emitted = 0;
  };
  WireStats wire_stats() const;

  // The venue's own words for a refused request, empty when none arrived.
  // Carried out of the callback thread so a run that recorded nothing can print
  // the reason instead of a timestamp.
  std::string last_rejection() const;

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

  // contracts -> base coin. the ONLY place this conversion happens: applied to
  // the tape and to every book level, so the .mkn file carries base coin and
  // nothing downstream needs to know which venue produced it.
  //
  // the multiplier is per-instrument (contract_to_base_, resolved from the
  // symbol), never a global constant: "one contract" is a different BTC amount
  // on every instId, and a shared constant silently mis-scales the next symbol
  // someone records.
  double ToBase(double size) const;
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
  // set once a refused handshake has been reported through EmitError, so a
  // socket that keeps being rejected does not tell its owner the same thing N
  // times -- and so the transient-error path stays untouched.
  std::atomic<bool> fatal_reported_{false};

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

  // base coins per contract for symbol_, resolved once from the per-instrument
  // table. 0 means "this build does not know this instrument's contract size",
  // and Start() treats that as fatal: running ETH-USDT through BTC's 0.001
  // writes a file that looks complete and is wrong by a constant factor.
  double contract_to_base_ = 0.0;

  // wire tallies: written on the websocket callback thread, read by the owner
  // after Stop() (or, for the recorder's summary, after the stream has ended).
  std::atomic<uint64_t> wire_opens_{0};
  std::atomic<uint64_t> wire_books_{0};
  std::atomic<uint64_t> wire_trades_{0};
  std::atomic<uint64_t> wire_control_{0};
  std::atomic<uint64_t> wire_unparsable_{0};
  std::atomic<uint64_t> wire_l2_emitted_{0};
  std::atomic<uint64_t> wire_trades_emitted_{0};

  // the rejection text is a string, so it cannot be an atomic: written on the
  // callback thread, read from the owning thread once the stream has stopped.
  mutable std::mutex rejection_mutex_;
  std::string last_rejection_;
};
