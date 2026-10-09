#pragma once
#include "trade_event.hpp"
#include <functional>

// abstract market data source for live forward-testing.
// concrete implementations (e.g. a binance websocket client) push
// data into the engine via the registered callbacks.
class IMarketStreamer {
public:
  // callback signatures used to hand data to the orchestrator.
  using TradeCallback = std::function<void(const TradeEvent&)>;
  using L2Callback    = std::function<void(const L2Snapshot&)>;

  virtual ~IMarketStreamer() = default;

  // register the sink for trade events (called from the streamer thread).
  void SetTradeCallback(TradeCallback cb) { on_trade_ = std::move(cb); }

  // register the sink for l2 snapshots (called from the streamer thread).
  void SetL2Callback(L2Callback cb) { on_l2_ = std::move(cb); }

  // begin streaming. must be non-blocking: spawn internal threads if needed.
  virtual void Start() = 0;

  // stop streaming and join any internal threads. must be idempotent.
  virtual void Stop() = 0;

protected:
  // helpers for concrete streamers to emit data.
  void EmitTrade(const TradeEvent& t) { if (on_trade_) on_trade_(t); }
  void EmitL2(const L2Snapshot& s)     { if (on_l2_)    on_l2_(s);    }

private:
  TradeCallback on_trade_;
  L2Callback    on_l2_;
};
