#pragma once
#include "trade_event.hpp"
#include <functional>
#include <string>

// abstract market data source for live forward-testing and for replay.
// concrete implementations (websocket clients and the .mkn replay reader)
// push data into the engine via the registered callbacks.
//
// ordering guarantee: a concrete streamer emits trades and l2 snapshots as
// one strictly-ordered stream by exchange timestamp. if a trade and a book
// update share a timestamp, the trade is emitted first. triggers rely on
// this to decide whether the book changed before or after the print.
class IMarketStreamer {
public:
  // callback signatures used to hand data to the orchestrator.
  using TradeCallback    = std::function<void(const TradeEvent&)>;
  using L2Callback       = std::function<void(const L2Snapshot&)>;
  // natural end of input (replay eof, or a live stream that closed cleanly).
  // never fired by Stop(): user-initiated teardown is not completion.
  using FinishedCallback = std::function<void()>;
  // unrecoverable transport error. the streamer stops itself after this.
  using ErrorCallback    = std::function<void(const std::string&)>;

  virtual ~IMarketStreamer() = default;

  // register the sink for trade events (called from the streamer thread).
  void SetTradeCallback(TradeCallback cb) { on_trade_ = std::move(cb); }

  // register the sink for l2 snapshots (called from the streamer thread).
  void SetL2Callback(L2Callback cb) { on_l2_ = std::move(cb); }

  // register the completion and error sinks. each is invoked at most once,
  // and never concurrently with the other.
  void SetFinishedCallback(FinishedCallback cb) { on_finished_ = std::move(cb); }
  void SetErrorCallback(ErrorCallback cb)       { on_error_    = std::move(cb); }

  // begin streaming. must be non-blocking: spawn internal threads if needed.
  virtual void Start() = 0;

  // stop streaming and join any internal threads. must be idempotent.
  virtual void Stop() = 0;

protected:
  // helpers for concrete streamers to emit data.
  void EmitTrade(const TradeEvent& t)  { if (on_trade_)    on_trade_(t);    }
  void EmitL2(const L2Snapshot& s)     { if (on_l2_)       on_l2_(s);       }
  void EmitFinished()                  { if (on_finished_) on_finished_();  }
  void EmitError(const std::string& m) { if (on_error_)    on_error_(m);    }

private:
  TradeCallback    on_trade_;
  L2Callback       on_l2_;
  FinishedCallback on_finished_;
  ErrorCallback    on_error_;
};
