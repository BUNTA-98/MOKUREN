
#pragma once
#include "aggregator.hpp"
#include "market_context.hpp"
#include <algorithm>
#include <ctime>
#include <vector>

class IFilter {
public:
  virtual ~IFilter() = default;
  virtual bool AllowTrade(const MarketContext &context,
                          TradeSignal intended_signal) = 0;
};

class MinVolumeFilter : public IFilter {
private:
  double min_volume_;

public:
  MinVolumeFilter(double min_volume) : min_volume_(min_volume) {}

  bool AllowTrade(const MarketContext &context,
                  TradeSignal intended_signal) override {
    if (context.history.empty())
      return false;
    // Blockiert den Trade, wenn die letzte Kerze extrem wenig Volumen hatte
    // (Choppiness)
    return context.history.back().total_volume >= min_volume_;
  }
};

//RELATIVE VOLUME
class RVOLFilter : public IFilter {
private:
  double rvol_threshold_;
  int period_;

public:
  // Standardmäßig fordern wir 150 % (1.5) des durchschnittlichen Volumens der letzten 20 Kerzen
  RVOLFilter(double rvol_threshold = 1.5, int period = 20)
      : rvol_threshold_(rvol_threshold), period_(period) {}

  bool AllowTrade(const MarketContext &context, TradeSignal intended_signal) override {
    // Wir benötigen N vorherige Kerzen + die aktuelle Kerze
    if (context.history.size() < period_ + 1) return false;

    double volume_sum = 0.0;
    size_t history_size = context.history.size();

    // 1. SMA des Volumens der letzten N Kerzen berechnen (OHNE die aktuelle)
    for (size_t i = history_size - 1 - period_; i < history_size - 1; ++i) {
      volume_sum += context.history[i].total_volume;
    }

    double avg_volume = volume_sum / period_;
    
    // Schutz vor Division durch Null bei extrem illiquiden Phasen
    if (avg_volume == 0.0) return false; 

    // 2. Relatives Volumen der aktuellsten Kerze ermitteln
    double current_volume = context.history.back().total_volume;
    double current_rvol = current_volume / avg_volume;

    // Trade nur erlauben, wenn das aktuelle Volumen den Schwellenwert sprengt
    return current_rvol >= rvol_threshold_;
  }
};

//ATR CHOP COP
class ATRFilter : public IFilter {
private:
  double min_atr_;
  int period_;

public:
  // Beispiel: Wir fordern mindestens 20$ durchschnittliche Bewegung über 14 Kerzen
  ATRFilter(double min_atr = 20.0, int period = 14)
      : min_atr_(min_atr), period_(period) {}

  bool AllowTrade(const MarketContext &context, TradeSignal intended_signal) override {
    // Wir benötigen N Kerzen + 1 vorherige für die True-Range-Berechnung
    if (context.history.size() < period_ + 1) return false;

    double atr_sum = 0.0;
    size_t size = context.history.size();

    // Average True Range (ATR) der letzten N Kerzen berechnen
    for (size_t i = size - period_; i < size; ++i) {
      double high_low = context.history[i].high - context.history[i].low;
      double high_close = std::abs(context.history[i].high - context.history[i - 1].close);
      double low_close = std::abs(context.history[i].low - context.history[i - 1].close);
      
      // True Range ist das Maximum dieser drei Werte
      double true_range = std::max({high_low, high_close, low_close});
      atr_sum += true_range;
    }

    double current_atr = atr_sum / period_;
    
    // Trade blockieren, wenn die Volatilität das Minimum nicht erreicht
    return current_atr >= min_atr_;
  }
};



//TIME OF DAY 
class TimeOfDayFilter : public IFilter {
private:
  int start_hour_, start_min_;
  int end_hour_, end_min_;

public:
  TimeOfDayFilter(int start_h, int start_m, int end_h, int end_m)
      : start_hour_(start_h), start_min_(start_m), end_hour_(end_h),
        end_min_(end_m) {}

  bool AllowTrade(const MarketContext &context,
                  TradeSignal intended_signal) override {
    if (context.history.empty())
      return false;
    
    // Unix-Timestamp der Kerze in lokale Zeit umwandeln
    std::time_t time_val = context.history.back().timestamp_start / 1000;
    
    // Eigene, thread-sichere Struktur anlegen
    struct tm time_info; 
    
    // localtime_r schreibt das Ergebnis direkt in unsere Variable
    localtime_r(&time_val, &time_info); 

    int current_mins_of_day = time_info.tm_hour * 60 + time_info.tm_min;
    int start_mins_of_day = start_hour_ * 60 + start_min_;
    int end_mins_of_day = end_hour_ * 60 + end_min_;
    
    // Prüfen, ob die aktuelle Zeit im Zeitfenster liegt
    return (current_mins_of_day >= start_mins_of_day &&
            current_mins_of_day <= end_mins_of_day);
  }
};

class POCTrendFilter : public IFilter {
public:
  bool AllowTrade(const MarketContext &context,
                  TradeSignal intended_signal) override {
    if (context.history.empty())
      return false;

    const Bar &last_bar = context.history.back();

    // Wenn kein POC berechnet wurde (sollte nicht passieren), erlaube den Trade
    if (last_bar.poc_price == 0.0)
      return true;

    if (intended_signal.direction == SignalDirection::BUY) {
      // Nur kaufen, wenn wir über dem stärksten Volumenknoten geschlossen haben
      return last_bar.close > last_bar.poc_price;
    } else if (intended_signal.direction == SignalDirection::SELL) {
      // Nur shorten, wenn wir unter dem stärksten Volumenknoten geschlossen
      // haben
      return last_bar.close < last_bar.poc_price;
    }

    return false;
  }
};

class MacroTrendFilter : public IFilter {
private:
  int period_;

public:
  // Standardmäßig prüfen wir den 200-Perioden-Trend (200 SMA)
  MacroTrendFilter(int period = 200) : period_(period) {}

  bool AllowTrade(const MarketContext &context, TradeSignal signal) override {
    // Wenn wir noch nicht genug Kerzen für den SMA haben, handeln wir nicht
    if (context.history.size() < period_) return false;

    double sum = 0.0;
    // Berechne den Durchschnitt der letzten N Kerzen
    for (size_t i = context.history.size() - period_; i < context.history.size(); ++i) {
      sum += context.history[i].close;
    }
    double sma = sum / period_;
    double current_price = context.history.back().close;

    // Nur LONG, wenn der Preis über dem SMA liegt
    if (signal.direction == SignalDirection::BUY && current_price < sma) {
      return false;
    }
    
    // Nur SHORT, wenn der Preis unter dem SMA liegt
    if (signal.direction == SignalDirection::SELL && current_price > sma) {
      return false;
    }

    return true;
  }
};

class VolatilityFilter : public IFilter {
private:
  double min_avg_range_;
  int period_;

public:
  // benötigt durchschnittlich 50$ kerzen-bewegung (high bis low) über die letzten 5 kerzen
  VolatilityFilter(double min_range = 50.0, int period = 5)
      : min_avg_range_(min_range), period_(period) {}

  bool AllowTrade(const MarketContext &context,
                  TradeSignal intended_signal) override {
    // wir brauchen genug kerzen für die berechnung
    if (context.history.size() < period_)
      return false;

    double total_range = 0.0;
    
    // summiere die spanne der letzten kerzen auf
    for (size_t i = context.history.size() - period_; i < context.history.size(); ++i) {
      total_range += (context.history[i].high - context.history[i].low);
    }

    double avg_range = total_range / period_;
    
    // erlaube trade nur, wenn der markt sich durchschnittlich stark genug bewegt
    return avg_range >= min_avg_range_;
  }
};
