#include "live_engine.hpp"

#include "market_context.hpp"

#include <utility>

LiveOrchestrator::LiveOrchestrator(EngineInstance engine,
                                   std::unique_ptr<IMarketStreamer> streamer,
                                   double sl_pct, double tp_pct)
    : engine_(std::move(engine)),
      streamer_(std::move(streamer)),
      sl_pct_(sl_pct),
      tp_pct_(tp_pct) {
  LOG_INFO("LiveOrchestrator constructed (queue cap={} sl_pct={} tp_pct={})",
           kMaxQueueDepth, sl_pct_, tp_pct_);
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
  if (engine_.ptrader) {
    double final_price = live_bar_.close;
    if (final_price <= 0.0) {
      final_price = engine_.aggregator
                        ? engine_.aggregator->GetLatestL2().best_bid_price
                        : 0.0;
    }
    if (final_price > 0.0) {
      LOG_INFO("LiveOrchestrator: force-closing open position at {}",
               final_price);
      engine_.ptrader->CloseOpenPositionAtEnd(final_price,
                                              live_bar_.timestamp_start);
    } else {
      LOG_WARN("LiveOrchestrator: no valid price to force-close position");
    }
  }

  LOG_INFO("LiveOrchestrator: stopped (dropped={})",
           dropped_.load(std::memory_order_relaxed));
}

void LiveOrchestrator::OnTrade(const TradeEvent& t) {
  Event ev;
  ev.type = Event::Type::TRADE;
  ev.trade = t;
  Push(std::move(ev));
}

void LiveOrchestrator::OnL2(const L2Snapshot& s) {
  Event ev;
  ev.type = Event::Type::L2;
  ev.l2 = s;
  Push(std::move(ev));
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

    if (final_sig.direction != SignalDirection::NONE && engine_.ptrader) {
      LOG_TRACE("LiveOrchestrator: executing signal dir={} entry={} sl={} tp={} vol={} ts={}",
                static_cast<int>(final_sig.direction), final_sig.entry_price,
                final_sig.stop_loss, final_sig.take_profit, final_sig.volume,
                ts);
      engine_.ptrader->ProcessSignal(final_sig, exec_price, ts);
    }
  };

  Event ev;
  while (Pop(ev)) {
    switch (ev.type) {
      case Event::Type::L2: {
        // feed both aggregators with the snapshot.
        if (engine_.aggregator) {
          engine_.aggregator->ProcessL2(ev.l2);
        }
        if (engine_.htf_aggregator) {
          engine_.htf_aggregator->ProcessL2(ev.l2);
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

        // 2. papertrader risk check (sl/tp/trail/scale-out triggers).
        if (engine_.ptrader) {
          engine_.ptrader->CheckRisk(ev.trade.price, ev.trade.timestamp);
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