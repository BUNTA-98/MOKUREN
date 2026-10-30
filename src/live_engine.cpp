#include "live_engine.hpp"

#include "live_debug.hpp"
#include "market_context.hpp"
#include "signal_executor.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

namespace {

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

const char* DirectionName(SignalDirection direction) {
  switch (direction) {
    case SignalDirection::BUY:  return "BUY";
    case SignalDirection::SELL: return "SELL";
    default:                    return "NONE";
  }
}

// the newest record in the trade log, plus the summed pnl of every record that
// shares its entry_time. a scale-out books one record per partial exit, so the
// last record alone reports only the final fragment of a partially closed
// trade. contiguous-only so two trades that happen to share a millisecond
// entry_time cannot bleed into each other.
struct ClosedTrade {
  bool found = false;
  TradeRecord last{};
  double pnl = 0.0;
};

ClosedTrade LastClosedTrade(const std::vector<TradeRecord>& history) {
  ClosedTrade out;
  if (history.empty()) return out;

  out.found = true;
  out.last = history.back();
  const int64_t entry_time = out.last.entry_time;
  for (auto it = history.rbegin(); it != history.rend(); ++it) {
    if (it->entry_time != entry_time) break;
    out.pnl += it->net_profit;
  }
  return out;
}

// fan a prepared event out to BOTH the existing pub/sub channel (telegram bot)
// and the persistent "mokuren:trades" stream (grafana), with one identical
// json payload.
void PublishJson(RedisPublisher* publisher, nlohmann::json& ev) {
  if (!publisher) return;
  publisher->PublishTradeEvent(ev.dump());
}

// The equity the prop guard measures total drawdown against is ACCOUNT STATE,
// so it is taken from the broker this run actually executes against instead of
// from a config field that could hold a second, silently different number.
//
//   --paper         PaperTrader, seeded from cfg.initial_balance by the factory
//                   on every path, so the base IS the funded account.
//   --live (venue)  the adapter's mark-to-market wallet equity. it is 0.0 until
//                   the private wallet stream delivers its first frame, so
//                   reading it as a base here only works on a brand-new
//                   account; the caller's account_baseline, when supplied,
//                   takes precedence precisely because of that.
//
// GetEquity(), not GetBalance(): on the venue path the wallet's totalEquity
// already carries the open position's floating pnl, and on the paper path the
// derived leg is net of the exit it would pay. GetBalance() is realised cash
// and would understate a loss taken while the position is still open.
double AccountStartingEquity(const EngineInstance& engine) {
  if (engine.broker) return engine.broker->GetEquity();
  if (engine.ptrader) return engine.ptrader->GetEquity();
  return 0.0;
}

} // namespace

LiveOrchestrator::LiveOrchestrator(EngineInstance engine,
                                   std::unique_ptr<IMarketStreamer> streamer,
                                   double sl_pct, double tp_pct,
                                   std::string redis_host, int redis_port,
                                   std::string symbol,
                                   mokuren::PropRules prop_rules,
                                   std::string venue,
                                   double account_baseline)
    : engine_(std::move(engine)),
      streamer_(std::move(streamer)),
      sl_pct_(sl_pct),
      tp_pct_(tp_pct),
      symbol_(std::move(symbol)),
      venue_(std::move(venue)),
      // the funded baseline wins over everything: it is the number the firm
      // measures total drawdown against, and unlike a venue reading it does not
      // move between restarts. without one, fall back to the broker's equity --
      // correct on a fresh account, and only then. engine_ is declared above
      // prop_guard_, so it is already moved into place here.
      prop_guard_(prop_rules,
                  account_baseline > 0.0
                      ? account_baseline
                      : AccountStartingEquity(engine_)) {
  LOG_INFO("LiveOrchestrator constructed (queue cap={} sl_pct={} tp_pct={} "
           "symbol={} venue={})",
           kMaxQueueDepth, sl_pct_, tp_pct_, symbol_,
           venue_.empty() ? "unknown" : venue_);

  // say out loud whether the account's survival rules are enforced. a prop run
  // with the guard off is the one configuration where a single bad day ends
  // the account, so it must never be silent.
  if (prop_rules.enabled) {
    // report the base actually in use. after seeding from the broker there is
    // no "initial_balance" anywhere in this process to print instead.
    LOG_INFO("LiveOrchestrator: prop guard armed (daily={}% total={}% "
             "warn_at={} of limit reset_hour={}z flatten={} base_equity={})",
             prop_rules.max_daily_drawdown_pct * 100.0,
             prop_rules.max_total_drawdown_pct * 100.0,
             prop_rules.warn_fraction, prop_rules.daily_reset_hour_utc,
             prop_rules.flatten_on_breach ? "on" : "off",
             prop_guard_.initial_equity() > 0.0
                 ? std::to_string(prop_guard_.initial_equity())
                 : std::string("derived from first equity reading"));
  } else {
    LOG_WARN("LiveOrchestrator: prop guard DISABLED -- no account-level "
             "drawdown limit is enforced on this run");
  }

  // The base is derived from the active broker now, so the old "funded balance
  // next to a paper account" mismatch is structurally impossible: --paper reads
  // back the same cfg.initial_balance the factory seeded PaperTrader with.
  // The one case still worth saying out loud is a venue whose wallet frame has
  // not landed yet.
  if (prop_rules.enabled && engine_.broker) {
    const double venue_equity = engine_.broker->GetEquity();
    if (account_baseline > 0.0) {
      // the base is known up front, so the guard measures from tick 0 and a
      // restart cannot reset it to a reduced equity.
      LOG_INFO("LiveOrchestrator: prop guard anchored to the funded baseline "
               "{} (venue equity right now: {})",
               account_baseline, venue_equity);
    } else if (!(venue_equity > 0.0)) {
      LOG_WARN("LiveOrchestrator: prop guard has no equity base yet (venue "
               "equity is {}) -- entries are refused until the wallet stream "
               "reports one, and the total-drawdown base will then be the "
               "FIRST reading it sees. On a restart mid-evaluation that base "
               "is already drawn down, so losses taken before this process "
               "started are not counted. Pass account_baseline to avoid this.",
               venue_equity);
    }
  }

  // start the async broadcaster. it owns its own worker thread and
  // never blocks the trading loop.
  redis_publisher_ = std::make_unique<RedisPublisher>(
      std::move(redis_host), redis_port, "mokuren:events");
}

void LiveOrchestrator::PublishOpenEvent(const TradeSignal& signal,
                                        int64_t timestamp) {
  if (!redis_publisher_) return;

  nlohmann::json ev;
  ev["timestamp"] = timestamp;
  ev["action"] = "TRADE_OPEN";
  ev["price"] = signal.entry_price;
  ev["size"] = signal.volume;
  ev["symbol"] = symbol_;
  ev["direction"] = DirectionName(signal.direction);
  ev["entry_price"] = signal.entry_price;
  ev["stop_loss"] = signal.stop_loss;
  ev["take_profit"] = signal.take_profit;
  ev["volume"] = signal.volume;

  PublishJson(redis_publisher_.get(), ev);
}

void LiveOrchestrator::PublishCloseEvent(double price, int64_t timestamp) {
  IBroker* broker = ActiveBroker();
  if (!broker || !redis_publisher_) return;

  nlohmann::json ev;
  ev["timestamp"] = timestamp;
  ev["action"] = "POSITION_CLOSED";
  ev["price"] = price;
  ev["size"] = broker->GetLastClosedSize();
  ev["symbol"] = symbol_;

  // pnl and exit_reason exist only in the trade log, and it returns by value
  // (live brokers append from their websocket thread) -- so take one copy and
  // read everything from it. an empty log still publishes the bare event: a
  // close notification with no pnl beats no notification at all.
  const std::vector<TradeRecord> history = broker->GetTradeHistory();
  const ClosedTrade closed = LastClosedTrade(history);
  if (closed.found) {
    ev["pnl"] = closed.pnl;
    ev["exit_reason"] = closed.last.exit_reason;
    ev["direction"] = DirectionName(closed.last.direction);
    ev["entry_price"] = closed.last.entry_price;
    ev["exit_price"] = closed.last.exit_price;
  }

  PublishJson(redis_publisher_.get(), ev);
}

LiveOrchestrator::~LiveOrchestrator() {
  // ensure threads are joined even if the caller forgot to call Stop().
  Stop();
}

bool LiveOrchestrator::Start() {
  // no startup checks: arm on the first frame. equivalent to the pre-gate
  // behaviour -- except that frames are still validated once the engine is
  // armed, and end-of-run errors still arrive through the router.
  return Start(mokuren::integrity::ProbeCriteria::Disabled());
}

bool LiveOrchestrator::Start(
    const mokuren::integrity::ProbeCriteria& criteria) {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
    LOG_WARN("LiveOrchestrator::Start called while already running");
    return false;
  }

  shutdown_.store(false, std::memory_order_release);
  dropped_.store(0, std::memory_order_relaxed);
  book_degraded_.store(false, std::memory_order_release);
  book_gap_.store(false, std::memory_order_release);
  stream_ended_.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stream_error_.clear();
  }

  // clear the per-run halt latches. when the caller supplied a baseline the
  // total-drawdown base survives, so a restart keeps measuring from the funded
  // balance instead of quietly re-deriving it from a reduced equity. without
  // one, Reset() returns to Unarmed and the base becomes the first positive
  // reading again -- the legacy behaviour, and the reason to pass it.
  prop_guard_.Reset();
  equity_base_cross_checked_ = false;
  last_prop_decision_ = mokuren::PropDecision{};
  // drop the previous run's gate so PreflightReport() cannot answer with a
  // stale verdict after a restart.
  router_.reset();

  if (!streamer_) {
    LOG_ERROR("LiveOrchestrator::Start: no streamer attached");
    running_.store(false, std::memory_order_release);
    return false;
  }

  // --- startup gate ---------------------------------------------------------
  // the router becomes the streamer's ONLY callback target, installed before
  // Start(): the connection that passes the checks is the connection that
  // trades, so no load balancer or rate limiter gets a second chance to behave
  // differently across a probe-then-reconnect gap.
  //
  // the finished/error sinks matter as much as the data ones: without them a
  // clean end of stream and a dropped socket both look like a market that went
  // quiet, and the worker waits on an empty queue forever.
  const std::string venue_name = venue_.empty() ? "unknown" : venue_;
  router_ = std::make_unique<mokuren::integrity::FeedRouter>(
      venue_name, mokuren::integrity::ProfileForVenue(venue_name), criteria,
      NowMillis());

  router_->SetLiveSink(mokuren::integrity::LiveSink{
      [this](const TradeEvent& t) { OnTrade(t); },
      [this](const L2Snapshot& s) { OnL2(s); },
      [this]() { OnStreamFinished(); },
      [this](const std::string& message) { OnStreamError(message); },
  });
  router_->Attach(*streamer_);

  // startup ping: lets us verify the redis integration without waiting
  // for the first trade to arrive.
  if (redis_publisher_) {
    nlohmann::json ping;
    ping["action"] = "SYSTEM_STARTUP";
    ping["msg"] = "Engine online";
    redis_publisher_->Publish(ping.dump());
  }

  if (engine_.broker) {
    // the balance is primed by ReconcileAccount() from REST during the
    // broker's construction, before this orchestrator exists, so on a healthy
    // startup GetBalance() is already the account's equity and the sizer has a
    // number from the first signal. the private wallet stream only refreshes
    // it -- waiting for that frame would have blocked trading on an account
    // whose equity is static.
    //
    // keep the zero case loud: a broker that skipped startup (directly
    // constructed in tests/tooling) leaves GetBalance() at 0 until the
    // `account` channel pushes, and every sizing path divides by it.
    const double venue_equity = engine_.broker->GetBalance();
    if (venue_equity > 0.0) {
      LOG_INFO("LiveOrchestrator: routing orders to the live venue adapter "
               "(equity={}, primed at startup); the wallet stream refreshes it",
               venue_equity);
    } else {
      LOG_WARN("LiveOrchestrator: routing orders to the live venue adapter "
               "(equity=0) -- no balance was primed at startup, so signals are "
               "suppressed until the wallet stream reports one");
    }
  } else if (engine_.ptrader) {
    // shadow / dry-run (--paper): real market data from the streamer, local
    // execution. say it once at start so a shadow run is never mistaken for a
    // live one in the audit log.
    LOG_INFO("LiveOrchestrator: SHADOW mode -- executing against the "
             "PaperTrader (equity={}); no orders will be sent to the venue",
             engine_.ptrader->GetBalance());
  }

  // one funnel, shared with every backtest path. the live-only gates (stale
  // book, prop guard) stay in execute_signal; everything from stop validation
  // to submission happens inside SignalExecutor.
  signal_executor_ = std::make_unique<mokuren::SignalExecutor>(
      ActiveBroker(), engine_.sizer.get(), engine_.risk_manager.get(),
      sl_pct_, tp_pct_, "live");

  LOG_INFO("LiveOrchestrator: starting worker thread");
  worker_ = std::thread(&LiveOrchestrator::WorkerLoop, this);

  // rates, not counts: print the threshold as the gate judges it, so this line
  // and the eventual verdict cannot describe two different criteria. depth is
  // included because it is now a startup precondition in its own right -- a
  // feed at the right rate but too shallow fails before the soak ends.
  LOG_INFO("LiveOrchestrator: starting market streamer (startup checks: "
           "trades>={}/min l2>={}/min depth>={} for >={}ms, timeout {}ms)",
           criteria.min_trades_per_min, criteria.min_l2_frames_per_min,
           criteria.min_l2_depth, criteria.min_duration_ms,
           criteria.timeout_ms);
  streamer_->Start();

  // --- wait for the gate, then arm -----------------------------------------
  // poll rather than spin: the gate enforces both the minimum soak and the hard
  // timeout itself, so this loop only has to notice the verdict.
  constexpr int64_t kPollIntervalMs = 20;
  constexpr int64_t kProgressIntervalMs = 5000;
  int64_t next_progress_ms = NowMillis() + kProgressIntervalMs;
  mokuren::integrity::ProbeState gate_state = router_->Poll(NowMillis());
  while (gate_state == mokuren::integrity::ProbeState::Probing) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    const int64_t now = NowMillis();
    gate_state = router_->Poll(now);

    // a one-minute soak is indistinguishable from a hang without this: the
    // process sits silent for 60s and then either prints a table or exits.
    // the counts come from the same report the caller prints on failure, so
    // the progress line and the verdict cannot disagree. the plain Start()
    // passes Disabled(), where Poll() is Ready on the first call, so this
    // never logs on the ungated path.
    if (gate_state == mokuren::integrity::ProbeState::Probing &&
        now >= next_progress_ms) {
      const mokuren::integrity::ProbeReport r = router_->Report();
      LOG_INFO("LiveOrchestrator: startup checks in progress ({} ms): "
               "trades={} l2={} rejected={}",
               r.elapsed_ms, r.trades.delivered, r.l2.delivered,
               r.trades.rejected() + r.l2.rejected());
      next_progress_ms = now + kProgressIntervalMs;
    }
  }

  // teardown for a run that never armed. deliberately not Stop(): that path
  // flushes the last candle and force-closes the position, and here nothing
  // traded. a flatten at a price taken from a book the gate just rejected
  // would book a phantom loss.
  auto abort_startup = [this]() {
    streamer_->Stop();
    RequestShutdown();
    if (worker_.joinable()) worker_.join();
    running_.store(false, std::memory_order_release);

    // drain the broadcaster before returning. Stop() cannot do it on this
    // path: it early-returns on running_ == false, and the destructor calls
    // it. the startup ping was queued before the checks ran, so without this
    // the one record saying "a process started and then refused to trade" is
    // the one that never reaches redis.
    if (redis_publisher_) {
      redis_publisher_->Flush();
    }
  };

  if (gate_state != mokuren::integrity::ProbeState::Ready) {
    const mokuren::integrity::ProbeReport report = router_->Report();
    LOG_ERROR("LiveOrchestrator: startup checks FAILED after {} ms: {}",
              report.elapsed_ms, report.reason);
    abort_startup();
    return false;
  }

  if (!router_->BeginRunning(NowMillis())) {
    LOG_ERROR("LiveOrchestrator: gate reported Ready but the router refused to "
              "arm (no live sink installed)");
    abort_startup();
    return false;
  }

  LOG_INFO("LiveOrchestrator: live run started");
  return true;
}

mokuren::integrity::ProbeReport LiveOrchestrator::PreflightReport() const {
  // the router outlives the probe on both outcomes: on Ready it keeps
  // validating every frame, on Failed the owner reads the reason here after
  // Start() returned false.
  return router_ ? router_->Report() : mokuren::integrity::ProbeReport{};
}

void LiveOrchestrator::Stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return; // already stopped
  }

  // shutdown ping: emitted before we tear anything down so the event
  // is guaranteed to be queued while the publisher is still alive.
  if (redis_publisher_) {
    nlohmann::json ping;
    ping["action"] = "SYSTEM_SHUTDOWN";
    ping["msg"] = "Engine offline";
    redis_publisher_->Publish(ping.dump());
  }

  LOG_INFO("LiveOrchestrator: stopping market streamer");
  if (streamer_) {
    streamer_->Stop();
  }

  // signal the worker to drain and exit.
  RequestShutdown();

  if (worker_.joinable()) {
    LOG_TRACE("LiveOrchestrator: joining worker thread");
    worker_.join();
  }

  // graceful shutdown: mirror the backtest end-of-run sequence.
  // flush the in-progress bar into history, then force-close any
  // open position at the last known price.
  if (engine_.aggregator) {
    LOG_INFO("LiveOrchestrator: flushing last candle on shutdown");
    engine_.aggregator->FlushLastCandle(live_bar_);
  }
  IBroker* broker = ActiveBroker();
  if (broker) {
    double final_price = live_bar_.close;
    if (final_price <= 0.0) {
      final_price = engine_.aggregator
                        ? engine_.aggregator->GetLatestL2().best_bid_price
                        : 0.0;
    }
    if (final_price > 0.0) {
      const bool was_open = broker->HasOpenPosition();

      if (broker == engine_.ptrader.get()) {
        // paper path: CloseOpenPositionAtEnd performs the full backtest
        // accounting (fees, stop history, win/loss bookkeeping). Flattening
        // with ClosePartial(1.0) would not reproduce backtest results.
        LOG_INFO("LiveOrchestrator: force-closing paper position at {}",
                 final_price);
        engine_.ptrader->CloseOpenPositionAtEnd(final_price,
                                                live_bar_.timestamp_start);
      } else {
        // venue path: IBroker has no end-of-run hook, so flatten the whole
        // position with a reduce-only close instead. ClosePartial is a no-op
        // when flat, so only announce a flatten when there is something to
        // flatten -- a no-trade run dropping the message read as if a
        // position had been closed on the way out.
        if (was_open) {
          LOG_INFO("LiveOrchestrator: flattening venue position at {}",
                   final_price);
        }
        broker->ClosePartial(1.0, final_price, live_bar_.timestamp_start,
                             "shutdown");
      }

      if (was_open) {
        PublishCloseEvent(final_price, live_bar_.timestamp_start);
      }
    } else {
      LOG_WARN("LiveOrchestrator: no valid price to force-close position");
    }
  }

  // block until the shutdown ping (and any other queued events) have
  // actually been written to redis before we return.
  if (redis_publisher_) {
    redis_publisher_->Flush();
  }

  // two distinct counts. dropped_ is queue overflow: frames the engine was
  // entitled to but could not keep up with. rejected_frames are frames the
  // router refused before they were ever queued (crossed books, stale
  // snapshots). a run that reconciles badly against the venue's own tape is
  // usually explained by the second number, not the first.
  LOG_INFO("LiveOrchestrator: stopped (dropped={} rejected_frames={})",
           dropped_.load(std::memory_order_relaxed),
           router_ ? router_->DroppedWhileRunning() : 0);
}

void LiveOrchestrator::OnTrade(const TradeEvent& t) {
  LIVE_DBG("orchestrator: OnTrade callback ts={} price={} qty={}",
           t.timestamp, t.price, t.quantity);

  // throttled live price ticker for grafana. non-blocking: the call only
  // enqueues (subject to the ~1000ms throttle) and returns immediately.
  if (redis_publisher_) {
    redis_publisher_->PublishTicker(symbol_, t.price, t.timestamp);
  }

  Event ev;
  ev.type = Event::Type::TRADE;
  ev.trade = t;
  bool ok = Push(std::move(ev));
  if (!ok) {
    LIVE_DBG("orchestrator: OnTrade DROPPED (queue full) ts={}", t.timestamp);
  }
}

void LiveOrchestrator::OnL2(const L2Snapshot& s) {
  LIVE_DBG("orchestrator: OnL2 callback ts={} bid={} ask={}",
           s.timestamp, s.best_bid_price, s.best_ask_price);
  Event ev;
  ev.type = Event::Type::L2;
  ev.l2 = s;
  bool ok = Push(std::move(ev));
  if (!ok) {
    LIVE_DBG("orchestrator: OnL2 DROPPED (queue full) ts={}", s.timestamp);
  }
}

void LiveOrchestrator::OnStreamFinished() {
  LOG_INFO("LiveOrchestrator: market stream finished (clean end of input)");
  stream_ended_.store(true, std::memory_order_release);
  RequestShutdown();
}

void LiveOrchestrator::OnStreamError(const std::string& message) {
  LOG_ERROR("LiveOrchestrator: market stream error: {}", message);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stream_error_ = message;
  }
  stream_ended_.store(true, std::memory_order_release);
  RequestShutdown();
}

void LiveOrchestrator::RequestShutdown() {
  // never joins here: this can run on the streamer's thread via the finished /
  // error callbacks, and joining it from inside would deadlock.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_.store(true, std::memory_order_release);
  }
  cv_.notify_all();
}

void LiveOrchestrator::CheckPropGuard(int64_t now_ms, double price) {
  IBroker* broker = ActiveBroker();
  if (!broker) return;
  CheckPropGuardEquity(now_ms, broker, price, broker->GetEquityAt(price));
}

void LiveOrchestrator::CheckPropGuardBidAsk(int64_t now_ms, double bid, double ask) {
  IBroker* broker = ActiveBroker();
  if (!broker) return;
  // the executable close for the open side: bid for a long, ask for a short.
  // equity and flatten price come from the same quote, so the guard and the
  // flatten tell the same story about the same book. best_bid was used
  // unconditionally before, which understated a short's floating loss by the
  // spread.
  const bool is_short =
      broker->GetCurrentPosition().direction == SignalDirection::SELL;
  const double close_price = is_short ? ask : bid;
  CheckPropGuardEquity(now_ms, broker, close_price,
                       broker->GetEquityAtBidAsk(bid, ask));
}

void LiveOrchestrator::CheckPropGuardEquity(int64_t now_ms, IBroker* broker,
                                            double close_price, double equity) {
  // mark-to-market equity at the price this frame carries, asked of the broker
  // rather than assembled here. the venue ignores the argument -- its wallet
  // push already holds the open position's floating pnl -- while the paper
  // account derives the leg from this mark, net of the exit fee and slippage
  // the position would pay. building a third version inline (realised balance
  // plus a gross floating leg, fees ignored) is how the live guard ended up
  // the most optimistic of the three and stayed blind to a loss that was open
  // but not yet booked.

  // one-shot sanity check: the guard's base and the venue's own equity must
  // describe the same account. a config carrying the paper default (10000) next
  // to a smaller funded account scales every limit by the ratio, and nothing
  // else would ever say so -- the guard would simply report a fraction of the
  // drawdown the firm measures.
  if (!equity_base_cross_checked_ && equity > 0.0 &&
      prop_guard_.initial_equity() > 0.0) {
    equity_base_cross_checked_ = true;
    const double base = prop_guard_.initial_equity();
    const double drift = std::abs(equity - base) / base;
    if (drift > 0.5) {
      LOG_ERROR("prop guard: equity base {} disagrees with the account's "
                "current equity {} (off by {}%) -- every limit is being "
                "measured against the wrong capital; set initial_balance to "
                "the funded baseline",
                base, equity, drift * 100.0);
    }
  }
  const mokuren::PropDecision d = prop_guard_.Observe(equity, now_ms);

  // log transitions only: this runs on every print, and a warning repeated
  // thousands of times buries the one line that mattered.
  if (d.state != last_prop_decision_.state) {
    switch (d.state) {
      case mokuren::PropState::Unarmed:
        LOG_WARN("prop guard: {} (equity={})", d.reason, equity);
        break;
      case mokuren::PropState::Normal:
        LOG_INFO("prop guard: {} (equity={} daily_dd={}% total_dd={}%)",
                 d.reason, equity, prop_guard_.daily_drawdown() * 100.0,
                 prop_guard_.total_drawdown() * 100.0);
        break;
      case mokuren::PropState::Warning:
        LOG_WARN("prop guard: {} -- no new entries (daily_dd={}% total_dd={}%)",
                 d.reason, prop_guard_.daily_drawdown() * 100.0,
                 prop_guard_.total_drawdown() * 100.0);
        break;
      case mokuren::PropState::DailyHalted:
      case mokuren::PropState::TotalHalted:
        LOG_ERROR("prop guard: {} -- no new entries (daily_dd={}% total_dd={}%)",
                  d.reason, prop_guard_.daily_drawdown() * 100.0,
                  prop_guard_.total_drawdown() * 100.0);
        break;
    }
    last_prop_decision_ = d;
  }

  if (!d.should_flatten) return;

  // the breach ends the run's right to hold risk, so the position goes flat
  // now instead of waiting for the stop. flatten is emitted once, on the
  // transition: repeating it every print would spray reduce-only orders at a
  // venue that already closed the position.
  //
  // no closed trade exists yet, so this carries no pnl -- only the reason and
  // the price the flatten is about to use. the POSITION_CLOSED event from
  // PublishCloseEvent below reports the realised result of that flatten.
  if (redis_publisher_) {
    nlohmann::json halt;
    halt["timestamp"] = now_ms;
    halt["action"] = "PROP_HALT";
    halt["price"] = close_price;
    halt["symbol"] = symbol_;
    halt["reason"] = d.reason;
    PublishJson(redis_publisher_.get(), halt);
  }
  LOG_ERROR("prop guard: {} -- flattening the open position at {}",
            d.reason, close_price);

  double flatten_price = close_price;
  if (!(flatten_price > 0.0) && engine_.aggregator) {
    flatten_price = engine_.aggregator->GetLatestL2().best_bid_price;
  }
  if (!(flatten_price > 0.0)) {
    LOG_ERROR("prop guard: no usable price to flatten at; the venue-side stop "
              "remains the only protection on this position");
    return;
  }

  const bool was_open = broker->HasOpenPosition();
  broker->ClosePartial(1.0, flatten_price, now_ms, "prop_guard");
  if (was_open) {
    PublishCloseEvent(flatten_price, now_ms);
  }
}

bool LiveOrchestrator::Push(Event&& ev) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= kMaxQueueDepth) {
      // drop the newest event to keep the queue bounded.
      size_t n = dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
      // latch the degraded state: from here the book is no longer a faithful
      // picture of the venue, so entries stop. this used to be a counter only,
      // which meant the engine kept opening positions against a stale book.
      if (!book_degraded_.exchange(true, std::memory_order_acq_rel)) {
        LOG_ERROR("LiveOrchestrator: event queue overflow -- the local book is "
                  "now stale; refusing new entries for the rest of the run");
      }
      // log every 1024th drop to avoid spamming the audit log.
      if ((n & 0x3FF) == 1) {
        LOG_WARN("LiveOrchestrator: queue overflow, dropped {} events so far",
                 n);
      }
      return false;
    }
    queue_.push_back(std::move(ev));
  }
  cv_.notify_one();
  return true;
}

bool LiveOrchestrator::Pop(Event& out) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] {
    return !queue_.empty() || shutdown_.load(std::memory_order_acquire);
  });

  if (queue_.empty()) {
    // shutdown requested and queue drained.
    return false;
  }

  out = std::move(queue_.front());
  queue_.pop_front();
  return true;
}

void LiveOrchestrator::WorkerLoop() {
  LOG_INFO("LiveOrchestrator: worker thread online");

  // mirrors the backtest loop in src/mokuren.cpp:
  //   - l2 snapshots are processed first (tick-level alpha evaluation)
  //   - then the trade is fed to pos_manager, ptrader, and both aggregators
  //   - finally the candle-level alpha is evaluated
  auto execute_signal = [this](TradeSignal raw_sig, double exec_price,
                               int64_t ts) {
    if (raw_sig.direction == SignalDirection::NONE) return;

    // no new risk on a book we know is stale. existing positions are untouched:
    // their hard stop sits on the venue and exits still run through
    // PositionManager / CheckRisk. two distinct latches feed this gate:
    //   - book_degraded_ (queue overflow) is terminal for the run.
    //   - book_gap_ (streamer reconnect / transport blip) clears on the next
    //     real snapshot, so entries resume once the book is faithful again.
    if (BookDegraded() || BookGap()) {
      LOG_ERROR("live: refusing a {} signal: the local book is stale "
                "(degraded={} gap={})",
                raw_sig.direction == SignalDirection::BUY ? "long" : "short",
                BookDegraded(), BookGap());
      return;
    }

    const bool is_long = raw_sig.direction == SignalDirection::BUY;

    // the account's survival limits outrank the strategy. this sits after the
    // stale-book gate because a stale book is the more immediate reason to
    // stand down, and before sizing because there is no point sizing a trade
    // that will not be sent.
    if (!prop_guard_.allow_new_entry()) {
      LOG_WARN("live: refusing a {} signal: prop guard state {} ({})",
               is_long ? "long" : "short",
               static_cast<int>(prop_guard_.state()),
               last_prop_decision_.reason);
      return;
    }

    // everything from stop validation to submission lives in the shared
    // funnel, so live and backtest cannot drift here any more.
    if (!signal_executor_) {
      LOG_ERROR("live: no signal executor attached; dropping {} signal",
                is_long ? "long" : "short");
      return;
    }

    const mokuren::SignalExecutionResult result =
        signal_executor_->Execute(raw_sig, exec_price, ts);

    // broadcast a trade-open event when a new position is established.
    if (result.opened_position) {
      PublishOpenEvent(result.signal, ts);
    }
  };

  Event ev;
  while (Pop(ev)) {
    LIVE_DBG("orchestrator: worker popped event type={}",
             ev.type == Event::Type::TRADE ? "TRADE" : "L2");
    switch (ev.type) {
      case Event::Type::L2: {
        // feed both aggregators with the snapshot.
        if (engine_.aggregator) {
          engine_.aggregator->ProcessL2(ev.l2);
        }
        if (engine_.htf_aggregator) {
          engine_.htf_aggregator->ProcessL2(ev.l2);
        }

        // book-gap sentinel: a frame with no bid or no ask is the streamer
        // telling us the local book is no longer a faithful picture of the
        // venue (reconnect, unexpected close, transport error). latch the gap
        // and skip the rest of this frame: a zeroed price would fake a
        // catastrophic drawdown on the paper path, and evaluating alpha
        // against a zeroed book is worse than not evaluating at all.
        if (ev.l2.best_bid_price <= 0.0 || ev.l2.best_ask_price <= 0.0) {
          if (!book_gap_.exchange(true, std::memory_order_acq_rel)) {
            LOG_WARN("LiveOrchestrator: book gap signalled by streamer; "
                     "entries suppressed until depth recovers");
          }
          break;
        }
        // a real snapshot arrived: the book is faithful again, so clear the
        // gap latch and let entries resume. book_degraded_ is untouched --
        // queue overflow is terminal for the run.
        if (book_gap_.exchange(false, std::memory_order_acq_rel)) {
          LOG_INFO("LiveOrchestrator: book recovered; entries re-enabled");
        }

        // account guard on the book path too. this used to run only in the
        // TRADE branch, so on a quiet book a floating loss could cross the
        // daily limit without the guard noticing -- and, now that the
        // backtest/WFA/replay paths observe on every L2 snapshot, leaving it
        // here would make live the one path that misses a breach.
        CheckPropGuardBidAsk(ev.l2.timestamp,
                             ev.l2.best_bid_price, ev.l2.best_ask_price);

        // guard: the bar must already be initialized by a real trade.
        // evaluating with a 0.0 close feeds garbage into the strategy.
        if (live_bar_.close == 0.0) {
          LIVE_DBG("orchestrator: skipping tick alpha, live_bar_ not "
                   "initialized (close=0.0)");
          break;
        }

        // tick-level alpha evaluation (mirrors backtest l2 catch-up loop).
        if (engine_.alpha && engine_.aggregator) {
          MarketContext tick_ctx{
              engine_.aggregator->GetHistory(),
              live_bar_,
              htf_live_bar_,
              engine_.aggregator->GetSessionMetrics(),
              engine_.aggregator->GetLatestL2(),
              engine_.aggregator->GetL2History(),
          };
          TradeSignal tick_sig = engine_.alpha->Evaluate(tick_ctx, false);
          LIVE_DBG("orchestrator: alpha->Evaluate(tick) returned direction={}",
                   static_cast<int>(tick_sig.direction));
          if (tick_sig.direction != SignalDirection::NONE) {
            double exec_price = (tick_sig.direction == SignalDirection::BUY)
                                    ? ev.l2.best_ask_price
                                    : ev.l2.best_bid_price;
            if (exec_price <= 0.0) {
              exec_price = live_bar_.close > 0.0 ? live_bar_.close
                                                 : ev.l2.best_bid_price;
            }
            LOG_TRACE("LiveOrchestrator: tick signal dir={} exec={} ts={}",
                      static_cast<int>(tick_sig.direction), exec_price,
                      ev.l2.timestamp);
            execute_signal(tick_sig, exec_price, ev.l2.timestamp);
          }
        }
        break;
      }

      case Event::Type::TRADE: {
        // the account guard runs before anything can act on this print: a
        // breach must refuse the trade, not race it. on the venue path the
        // equity behind it arrives asynchronously on the private wallet
        // stream, so this reads the most recent value the broker holds.
        CheckPropGuard(ev.trade.timestamp, ev.trade.price);

        // 1. position manager update (trailing / be / scale-out bookkeeping).
        if (engine_.pos_manager) {
          engine_.pos_manager->Update(ev.trade.price, ev.trade.timestamp);
        }

        // 2. active-broker risk check (sl/tp/trail/scale-out triggers).
        IBroker* broker = ActiveBroker();
        if (broker) {
          bool was_open = broker->HasOpenPosition();
          broker->CheckRisk(ev.trade.price, ev.trade.timestamp);
          bool is_open = broker->HasOpenPosition();

          // broadcast a position-closed event when the risk check
          // (sl/tp/trail/scale-out) fully closes the position.
          if (was_open && !is_open) {
            PublishCloseEvent(ev.trade.price, ev.trade.timestamp);
          }
        }

        // 3. build the live bar (and htf bar) from the trade.
        bool candle_finished = false;
        if (engine_.aggregator) {
          candle_finished =
              engine_.aggregator->ProcessTrade(live_bar_, ev.trade);
        }
        if (engine_.htf_aggregator) {
          engine_.htf_aggregator->ProcessTrade(htf_live_bar_, ev.trade);
        }
        LIVE_DBG("orchestrator: ProcessTrade done ts={} candle_finished={} live_bar_ts={} live_bar_close={}",
                 ev.trade.timestamp, candle_finished,
                 live_bar_.timestamp_start, live_bar_.close);

        // guard: never evaluate a candle that was never initialized by
        // a real trade (close stays 0.0).
        if (live_bar_.close == 0.0) {
          LIVE_DBG("orchestrator: skipping candle alpha, live_bar_ not "
                   "initialized (close=0.0)");
          break;
        }

        // 4. candle-level alpha evaluation.
        if (engine_.alpha && engine_.aggregator) {
          MarketContext ctx{
              engine_.aggregator->GetHistory(),
              live_bar_,
              htf_live_bar_,
              engine_.aggregator->GetSessionMetrics(),
              engine_.aggregator->GetLatestL2(),
              engine_.aggregator->GetL2History(),
          };
          TradeSignal candle_sig = engine_.alpha->Evaluate(ctx, candle_finished);
          LIVE_DBG("orchestrator: alpha->Evaluate(candle) returned direction={} candle_finished={}",
                   static_cast<int>(candle_sig.direction), candle_finished);
          if (candle_sig.direction != SignalDirection::NONE) {
            LOG_TRACE("LiveOrchestrator: candle signal dir={} ts={}",
                      static_cast<int>(candle_sig.direction),
                      ev.trade.timestamp);
            execute_signal(candle_sig, ev.trade.price, ev.trade.timestamp);
          }
        }
        break;
      }
    }
  }

  LOG_INFO("LiveOrchestrator: worker thread exiting");
}
