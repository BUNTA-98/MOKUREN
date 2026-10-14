#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// opaque forward declarations so this header stays free of the heavy C
// headers (notcurses, hiredis) that only tui_dashboard.cpp needs.
struct notcurses;
struct ncplane;
struct redisContext;
struct redisReply;

namespace mokuren::tui {

// which way a freshly executed trade leaned.
enum class MarkerSide { Unknown, Buy, Sell };

// one trade execution to be overlaid onto the price plot.
struct TradeMarker {
  double price = 0.0;
  int64_t timestamp_ms = 0;
  MarkerSide side = MarkerSide::Unknown;
  // monotonic ticker sequence number at the moment of arrival. used as the
  // x-axis anchor so the marker stays glued to the right spot while the
  // price window scrolls.
  int64_t seq = 0;
};

// thread-safe shared state between the redis worker thread and the render
// thread. all access is serialised by an internal mutex.
class UiState {
public:
  static constexpr size_t kMaxTicks = 120;    // ~100 recent ticker prices
  static constexpr size_t kMaxMarkers = 256;  // recent trade markers

  // a consistent copy of the whole state, taken under the lock.
  struct Snapshot {
    std::deque<double> ticks;
    std::deque<TradeMarker> markers;
    int64_t tick_seq = 0;  // total ticks ever pushed (monotonic)
    std::string symbol = "?";
  };

  void PushTick(double price);
  void PushMarker(TradeMarker marker);  // seq is filled in internally
  void SetSymbol(const std::string& symbol);

  Snapshot Take() const;

private:
  mutable std::mutex mutex_;
  std::deque<double> ticks_;
  std::deque<TradeMarker> markers_;
  int64_t tick_seq_ = 0;
  std::string symbol_ = "?";
};

// live terminal dashboard. reads the redis streams published by
// RedisPublisher and renders them with notcurses. completely independent
// of the backtest UI.
class TuiDashboard {
public:
  struct Options {
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    int fps = 30;
  };

  TuiDashboard();
  explicit TuiDashboard(Options opts);
  ~TuiDashboard();

  TuiDashboard(const TuiDashboard&) = delete;
  TuiDashboard& operator=(const TuiDashboard&) = delete;

  // initialise notcurses, start the redis worker and run the render loop.
  // blocks until the user quits. returns a process exit code.
  int Run();

private:
  // --- redis worker ---------------------------------------------------
  void RedisWorker();
  bool ConnectRedis();
  bool WaitConnected(int timeout_ms);
  void DisconnectRedis();
  void ProcessReply(struct redisReply* reply);
  void ParseTickerEntry(struct redisReply* fields);
  void ParseTradeEntry(struct redisReply* fields);

  // --- rendering ------------------------------------------------------
  // cell-space rectangle occupied by the braille price plot.
  struct ChartGeometry {
    int x = 0;  // left column of the plot
    int y = 0;  // top row of the plot
    int w = 0;  // width in terminal cells
    int h = 0;  // height in terminal cells
  };

  void Layout();
  void RenderFrame();
  // draws the y/x axes, reference gridlines and a connected price line into
  // the braille grid, then overlays the trade markers.
  void RenderPriceChart(struct ncplane* plane,
                        const UiState::Snapshot& snap,
                        const ChartGeometry& geo);
  void DrawMarkers(struct ncplane* plane, const UiState::Snapshot& snap,
                   const ChartGeometry& geo, int64_t base_seq,
                   double miny, double maxy);

  Options opts_;
  UiState state_;

  std::thread redis_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> redis_connected_{false};
  struct redisContext* redis_ctx_ = nullptr;
  // XREAD cursor for each stream. "$" on the very first read, then the
  // last seen entry id so a reconnect resumes without dropping events.
  std::string next_id_ticker_ = "$";
  std::string next_id_trade_ = "$";

  struct notcurses* nc_ = nullptr;
};

}  // namespace mokuren::tui
