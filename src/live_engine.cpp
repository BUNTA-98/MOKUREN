#include "live_engine.hpp"

#include "live_debug.hpp"
#include "market_context.hpp"

#include <nlohmann/json.hpp>

#include <utility>

LiveOrchestrator::LiveOrchestrator(EngineInstance engine,
                                   std::unique_ptr<IMarketStreamer> streamer,
                                   double sl_pct, double tp_pct,
                                   std::string redis_host, int redis_port,
                                   std::string symbol)
    : engine_(std::move(engine)),
      streamer_(std::move(streamer)),
      sl_pct_(sl_pct),
      tp_pct_(tp_pct),
      symbol_(std::move(symbol)) {
  LOG_INFO("LiveOrchestrator constructed (queue cap={} sl_pct={} tp_pct={} symbol={})",
           kMaxQueueDepth, sl_pct_, tp_pct_, symbol_);

  // start the async broadcaster. it owns its own worker thread and
  // never blocks the trading loop.
  redis_publisher_ = std::make_unique<RedisPublisher>(
      std::move(redis_host), redis_port, "mokuren:events");
}

void LiveOrchestrator::PublishEvent(const char* action, double price,
                                    double size, int64_t timestamp) {
  if (!redis_publisher_) return;

  nlohmann::json ev;
  ev["timestamp"] = timestamp;
  ev["action"] = action;
  ev["price"] = price;
  ev["size"] = size;
  ev["symbol"] = symbol_;

  // fan out to BOTH the existing pub/sub channel (telegram bot) and the
  // persistent "mokuren:trades" stream (grafana). identical json payload.
  redis_publisher_->PublishTradeEvent(ev.dump());
}

LiveOrchestrator::~LiveOrchestrator() {
  // ensure threads are joined even if the caller forgot to call Stop().
  Stop();
}

bool LiveOrchestrator::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
    LOG_WARN("LiveOrchestrator::Start called while already running");
    return false;
  }

  shutdown_.store(false, std::memory_order_release);
  dropped_.store(0, std::memory_order_relaxed);

  if (!streamer_) {
    LOG_ERROR("LiveOrchestrator::Start: no streamer attached");
    running_.store(false, std::memory_order_release);
    return false;
  }

  // wire the streamer callbacks into our queue before starting it.
  streamer_->SetTradeCallback([this](const TradeEvent& t) { OnTrade(t); });
  streamer_->SetL2Callback([this](const L2Snapshot& s) { OnL2(s); });

  // startup ping: lets us verify the redis integration without waiting
  // for the first trade to arrive.
  if (redis_publisher_) {
    nlohmann::json ping;
    ping["action"] = "SYSTEM_STARTUP";
    ping["msg"] = "Engine online";
    redis_publisher_->Publish(ping.dump());
  }

  if (engine_.broker) {
    LOG_WARN("LiveOrchestrator: routing orders to the live venue adapter "
             "instead of the PaperTrader -- confirm the adapter's execution "
             "path is implemented before trusting this run");
  }

  LOG_INFO("LiveOrchestrator: starting worker thread");
  worker_ = std::thread(&LiveOrchestrator::WorkerLoop, this);

  LOG_INFO("LiveOrchestrator: starting market streamer");
  streamer_->Start();

  LOG_INFO("LiveOrchestrator: live run started");
  return true;
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
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_.store(true, std::memory_order_release);
  }
  cv_.notify_all();

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
        PublishEvent("POSITION_CLOSED", final_price,
                     broker->GetLastClosedSize(),
                     live_bar_.timestamp_start);
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

  LOG_INFO("LiveOrchestrator: stopped (dropped={})",
           dropped_.load(std::memory_order_relaxed));
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

bool LiveOrchestrator::Push(Event&& ev) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= kMaxQueueDepth) {
      // drop the newest event to keep the queue bounded.
      size_t n = dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
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

    raw_sig.entry_price = exec_price;
    raw_sig.stop_loss =
        exec_price * (raw_sig.direction == SignalDirection::BUY
                          ? (1.0 - sl_pct_)
                          : (1.0 + sl_pct_));
    raw_sig.take_profit =
        exec_price * (raw_sig.direction == SignalDirection::BUY
                          ? (1.0 + tp_pct_)
                          : (1.0 - tp_pct_));

    TradeSignal sized = raw_sig;
    if (engine_.sizer) {
      sized = engine_.sizer->CalculateSize(raw_sig);
    }

    TradeSignal final_sig = sized;
    if (engine_.risk_manager) {
      final_sig = engine_.risk_manager->Evaluate(sized, ts);
    }

    IBroker* broker = ActiveBroker();
    if (final_sig.direction != SignalDirection::NONE && broker) {
      LOG_TRACE("LiveOrchestrator: executing signal dir={} entry={} sl={} tp={} vol={} ts={}",
                static_cast<int>(final_sig.direction), final_sig.entry_price,
                final_sig.stop_loss, final_sig.take_profit, final_sig.volume,
                ts);

      bool was_open = broker->HasOpenPosition();
      broker->ProcessSignal(final_sig, exec_price, ts);
      bool is_open = broker->HasOpenPosition();

      // broadcast a trade-open event when a new position is established.
      if (!was_open && is_open) {
        PublishEvent("TRADE_OPEN", final_sig.entry_price, final_sig.volume, ts);
      }
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
            PublishEvent("POSITION_CLOSED", ev.trade.price,
                         broker->GetLastClosedSize(),
                         ev.trade.timestamp);
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
