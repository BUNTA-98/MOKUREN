#pragma once
#include "trade_event.hpp"
#include <vector>

// basis signal-typen für die order-engine
enum class SignalDirection { NONE, BUY, SELL };

// container für ein generiertes trade-signal
struct TradeSignal {
  SignalDirection direction = SignalDirection::NONE;
  double entry_price = 0.0;
  double stop_loss = 0.0;
  double take_profit = 0.0;
  double volume = 0.0;
};

// statisches limit für das preis-grid (footprint block)
constexpr int MAX_GRID_LEVELS = 5000;

// ein einzelnes preis-level im orderbuch/footprint
struct PriceLevel {
  double price = 0.0;
  double bid_volume = 0.0;
  double ask_volume = 0.0;
};

// die aggregierte kerze (ohlc + orderflow + l2)
struct Bar {
  double open = 0.0;
  double high = 0.0;
  double low = 0.0;
  double close = 0.0;

  double total_volume = 0.0;
  double cumulative_delta = 0.0;
  double poc_price = 0.0;
  double vwap = 0.0; // vwap wert am ende der kerze

  int64_t timestamp_start = 0;
  double tick_size = 0.0;

  std::vector<PriceLevel> footprint;
  L2Snapshot last_l2; // letzter bekannter orderbuch-stand für diese kerze

  // reset für die nächste zeiteinheit
  void ResetBar(int64_t new_timestamp) {
    open = 0.0;
    high = 0.0;
    low = 0.0;
    close = 0.0;
    total_volume = 0.0;
    cumulative_delta = 0.0;
    poc_price = 0.0;
    vwap = 0.0;
    timestamp_start = new_timestamp;
    footprint.clear();
    // l2 state wird absichtlich nicht genullt, da das buch fließend ist
  }
};

// laufende tages-metriken
struct SessionMetrics {
  double vwap = 0.0;
  double total_session_volume = 0.0;
};

class Aggregator {
public:
  // konstruktor mit config werten (interval, tick, vwap-offset)
  Aggregator(int64_t interval, double tick, int vwap_reset_hour = 0)
      : interval_ms(interval), tick_size(tick), vwap_reset_hour_(vwap_reset_hour) {
    ResetWorkspace();
  }

  PriceLevel *GetOrAddLevel(double price);
  bool ProcessTrade(Bar &live_bar, const TradeEvent &trade);
  void ProcessL2(const L2Snapshot &l2); // verarbeitet das neue l2-binary format
  void UpdateBarData(Bar &bar, const TradeEvent &trade);
  void AnalyzeCandle(Bar &bar); 
  
  const std::vector<Bar> &GetHistory() const { return history; }
  SessionMetrics GetSessionMetrics() const { return current_metrics_; }
  L2Snapshot GetLatestL2() const { return latest_l2_; }
  void FlushLastCandle(Bar &live_bar);

private:
  int64_t interval_ms;
  double tick_size;
  int vwap_reset_hour_ = 0; // stunden-offset für tageswechsel (z.b. ny open)
  int64_t next_close_time = 0;

  std::vector<Bar> history;

  // lokales ram-grid für den footprint um map-overhead zu sparen
  PriceLevel live_workspace[MAX_GRID_LEVELS];
  double base_price = 0.0;
  double lowest_price = 0.0;
  int active_levels = 0;

  // vwap state tracker
  int64_t current_session_day_ = -1; 
  double session_vol_ = 0.0;
  double session_price_vol_ = 0.0;
  SessionMetrics current_metrics_;

  // zuletzt gelesener l2-snapshot aus dem binär-stream
  L2Snapshot latest_l2_{};

  // leert das grid für eine neue kerze
  void ResetWorkspace() {
    base_price = 0.0;
    lowest_price = 0.0;
    active_levels = 0;
    for (int i = 0; i < MAX_GRID_LEVELS; i++) {
      live_workspace[i].price = 0.0;
      live_workspace[i].bid_volume = 0.0;
      live_workspace[i].ask_volume = 0.0;
    }
  }

  // verdichtet das offene grid in einen sauberen vektor für den speicher
  void PackFootprint(Bar &bar) {
    bar.footprint.clear();
    bar.footprint.reserve(active_levels);
    for (int i = 0; i < MAX_GRID_LEVELS; i++) {
      if (live_workspace[i].bid_volume > 0 || live_workspace[i].ask_volume > 0) {
        bar.footprint.push_back(live_workspace[i]);
      }
    }
  }
};
