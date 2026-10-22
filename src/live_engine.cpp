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

} // namespace

LiveOrchestrator::LiveOrchestrator(EngineInstance engine,
                                   std::unique_ptr<IMarketStreamer> streamer,
                                   double sl_pct, double tp_pct,
                                   std::string redis_host, int redis_port,
                                   std::string symbol,
                                   mokuren::PropRules prop_rules,
                                   std::string venue)
    : engine_(std::move(engine)),
      streamer_(std::move(streamer)),
      sl_pct_(sl_pct),
      tp_pct_(tp_pct),
      symbol_(std::move(symbol)),
      venue_(std::move(venue)),
      prop_guard_(prop_rules) {
  LOG_INFO("LiveOrchestrator constructed (queue cap={} sl_pct={} tp_pct={} "
           "symbol={} venue={})",
           kMaxQueueDepth, sl_pct_, tp_pct_, symbol_,
           venue_.empty() ? "unknown" : venue_);

  // say out loud whether the account's survival rules are enforced. a prop run
  // with the guard off is the one configuration where a single bad day ends
  // the account, so it must never be silent.
  if (prop_rules.enabled) {
    LOG_INFO("LiveOrchestrator: prop guard armed (daily={}% total={}% "
             "warn_at={} of limit reset_hour={}z flatten={} initial_balance={})",
             prop_rules.max_daily_drawdown_pct * 100.0,
             prop_rules.max_total_drawdown_pct * 100.0,
             prop_rules.warn_fraction, prop_rules.daily_reset_hour_utc,
             prop_rules.flatten_on_breach ? "on" : "off",
             prop_rules.initial_balance > 0.0
                 ? std::to_string(prop_rules.initial_balance)
                 : std::string("derived from first equity reading"));
  } else {
    LOG_WARN("LiveOrchestrator: prop guard DISABLED -- no account-level "
             "drawdown limit is enforced on this run");
  }

  // a funded balance next to a paper account halts instantly: 100k of starting
  // equity measured against a 10k paper balance is a 90% drawdown before the
  // first trade, and the guard would then refuse every entry in silence.
  // compare against the balance actually seeded into the paper account, not the
  // compile-time constant: --paper seeds it from cfg.initial_balance.
  if (prop_rules.enabled && !engine_.broker && engine_.ptrader &&
      prop_rules.initial_balance > 0.0) {
    const double paper_balance = engine_.ptrader->GetBalance();
    if (std::abs(prop_rules.initial_balance - paper_balance) > 1.0) {
      LOG_WARN("LiveOrchestrator: prop.initial_balance={} does not match the "
               "paper account's {} -- this run starts past the total drawdown "
               "limit and will refuse every entry",
               prop_rules.initial_balance, paper_balance);
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

  // clear the per-run halt latches. the total-drawdown base survives (it is a
  // config constant), so a restart keeps measuring from the funded balance
  // instead of quietly re-deriving it from a reduced equity.
  prop_guard_.Reset();
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
    // a venue adapter sizes from exchange equity, which only arrives on the
    // private wallet stream. until that frame lands GetBalance() is 0.0, the
    // sizer produces no volume, and the first signals are dropped. say it once
    // at start instead of leaving it to be inferred from silence.
    LOG_WARN("LiveOrchestrator: routing orders to the live venue adapter "
             "(equity={}) -- signals are suppressed until the wallet stream "
             "reports a balance",
             engine_.broker->GetBalance());
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

  LOG_INFO("LiveOrchestrator: starting market streamer (startup checks: "
           "trades>={} l2>={} for >={}ms, timeout {}ms)",
           criteria.min_trades, criteria.min_l2_frames,
           criteria.min_duration_ms, criteria.timeout_ms);
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
        // position with a reduce-only close instead.
        LOG_INFO("LiveOrchestrator: flattening venue position at {}",
                 final_price);
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

  // venue path: GetBalance() is mark-to-market equity (wallet totalEquity,
  // which includes open pnl) -- that is what the firm measures. it arrives
  // asynchronously on the private wallet stream.
  //
  // paper / shadow path: there is no wallet stream at all. balance only moves
  // when a trade closes, so the floating leg must be derived from the open
  // position here; measuring the closed balance alone made the guard report
  // realised drawdown and look safer than live while a losing trade was open.
  // entry/exit fees are not subtracted; on a 4-6% limit they are noise, and
  // CheckRisk already folds them into its own drawdown tracking.
  double equity = broker->GetBalance();
  if (engine_.ptrader && broker == engine_.ptrader.get() && price > 0.0) {
    const TradeSignal pos = broker->GetCurrentPosition();
    if (pos.volume > 0.0 && pos.entry_price > 0.0 &&
        pos.direction != SignalDirection::NONE) {
      const double dir = pos.direction == SignalDirection::BUY ? 1.0 : -1.0;
      equity += (price - pos.entry_price) * pos.volume * dir;
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
    halt["price"] = price;
    halt["symbol"] = symbol_;
    halt["reason"] = d.reason;
    PublishJson(redis_publisher_.get(), halt);
  }
  LOG_ERROR("prop guard: {} -- flattening the open position at {}",
            d.reason, price);

  double flatten_price = price;
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
        CheckPropGuard(ev.l2.timestamp,
                       ev.l2.best_bid_price > 0.0 ? ev.l2.best_bid_price
                                                  : ev.l2.best_ask_price);

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
