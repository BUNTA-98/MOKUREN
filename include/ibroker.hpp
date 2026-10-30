#pragma once
#include "aggregator.hpp"
#include <vector>
#include <string>

// represents a single adjustment of the stop loss
struct StopEvent {
    int64_t timestamp;
    size_t candle_idx;
    double sl_price;
    std::string type;
};

// core trade data protocol logged by the paper trader
struct TradeRecord {
  int64_t entry_time;
  int64_t exit_time;
  SignalDirection direction;
  double entry_price;
  double exit_price;
  double net_profit;
  std::string exit_reason;
  std::vector<StopEvent> stop_events;
};

// broker interface for execution
class IBroker {
public:
    virtual ~IBroker() = default;
    
    virtual double GetBalance() const = 0;
    // mark-to-market equity: balance plus the floating PnL of any open
    // position, net of fees. the prop guard measures drawdown against this,
    // never against the balance, because the firm's rule counts open
    // positions. a balance-only reading reports an open position's loss as
    // zero, so a breach stays invisible until the position closes -- which is
    // after the firm has already counted it.
    virtual double GetEquity() const = 0;
    // the same equity marked at an EXPLICIT price. the caller that holds the
    // current mark must not have to wait for the broker to be ticked with it
    // first: on the backtest paths the guard observes on the same print that
    // drives the bar, and GetEquity() would answer from the previous one.
    // a venue ignores the argument -- its wallet already carries the floating
    // pnl of every open position at the venue's own mark, so there is nothing
    // to re-price -- which is why this is not pure: an adapter that reports a
    // mark-to-market balance has already answered the question.
    virtual double GetEquityAt(double mark) const { (void)mark; return GetEquity(); }
    // mark-to-market equity with an executable close priced from BOTH sides
    // of the book: a long exits at the bid, a short at the ask. the ask is
    // the correct quote for a short and is strictly higher than the bid, so
    // marking a short at best_bid understated its floating loss by the
    // spread. callers that hold the whole book (the L2 catch-up in scan,
    // WFA, replay, and the live worker) must use this instead of GetEquityAt,
    // which treats its argument as a mid and slips it direction-aware --
    // correct for a trade print, wrong for a one-sided quote.
    //
    // a venue ignores both arguments: its wallet push already carries the
    // floating pnl at the venue's own mark, so there is nothing to re-price.
    // the default mirrors GetEquityAt for exactly that reason.
    virtual double GetEquityAtBidAsk(double bid, double ask) const {
        (void)bid;
        (void)ask;
        return GetEquity();
    }
    virtual bool HasOpenPosition() const = 0;
    virtual TradeSignal GetCurrentPosition() const = 0;
    virtual void UpdateStopLoss(double new_sl) = 0;

    // core execution hooks called by the orchestrator
    virtual void ProcessSignal(TradeSignal signal, double fill_price, int64_t current_time) = 0;
    virtual void CheckRisk(double current_price, int64_t current_time) = 0;
    
    // trigger partial exits for scale outs
    virtual void ClosePartial(double fraction, double current_price, int64_t current_time, const std::string& reason) = 0;
    
    // required for risk manager and engine state synchronization
    virtual double GetNetProfit() const = 0;
    // returns a copy of the log. live brokers append to it from their
    // websocket thread while the engine reads, so handing out a reference
    // would leave the caller iterating a vector that is being mutated.
    virtual std::vector<TradeRecord> GetTradeHistory() const = 0;
    // O(1) accessors: the risk firewall reads the last exit time and the log
    // length on every signal and must not copy the whole vector to do it.
    virtual int64_t GetLastTradeTime() const = 0;
    virtual size_t GetTradeCount() const = 0;

    // size of the most recently closed position (0.0 if none yet).
    // used by the live event broadcaster to report closed volume.
    virtual double GetLastClosedSize() const = 0;

    // liveness of the execution path. a venue adapter reports whether its
    // private stream is authenticated and delivering -- the one component that
    // can fail without the process exiting. the default is true because a
    // paper account has no socket to drop; defaulting false would make every
    // backtest, scan and WFA run report itself disconnected.
    virtual bool IsConnected() const { return true; }
};
