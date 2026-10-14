#include "tui_dashboard.hpp"

#include "logger.hpp"
#include "redis_publisher.hpp"

#include <hiredis/hiredis.h>
#include <nlohmann/json.hpp>
#include <notcurses/notcurses.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

namespace mokuren::tui {

namespace {

// how long poll() sleeps before the worker re-checks the shutdown flag.
constexpr int kPollTimeoutMs = 100;
// back-off between failed reconnection attempts.
constexpr int kReconnectDelayMs = 500;
// how long we wait for the non-blocking connect() to complete.
constexpr int kConnectTimeoutMs = 1000;

// --- defensive json extraction helpers (never throw on missing/mistyped) ---

double JsonNumber(const nlohmann::json& j,
                  std::initializer_list<const char*> keys, double fallback) {
  for (const char* key : keys) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number()) return it->get<double>();
  }
  return fallback;
}

int64_t JsonInt(const nlohmann::json& j,
                std::initializer_list<const char*> keys, int64_t fallback) {
  for (const char* key : keys) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number()) return it->get<int64_t>();
  }
  return fallback;
}

std::string JsonString(const nlohmann::json& j,
                       std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_string()) return it->get<std::string>();
  }
  return {};
}

MarkerSide SideFromString(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (value == "buy" || value == "long" || value == "bid") return MarkerSide::Buy;
  if (value == "sell" || value == "short" || value == "ask") return MarkerSide::Sell;
  return MarkerSide::Unknown;
}

// --- price-chart helpers ------------------------------------------------

// braille dot bit for (sub-column, sub-row) inside a single cell.
constexpr uint8_t kBrailleDots[2][4] = {
    {0x01, 0x02, 0x04, 0x40},  // left column:  dots 1,2,3,7
    {0x08, 0x10, 0x20, 0x80},  // right column: dots 4,5,6,8
};

// pick a sensible number of decimals for an axis spanning `range`.
int PriceDecimals(double range) {
  if (range >= 100.0) return 1;
  if (range >= 1.0) return 2;
  if (range >= 0.01) return 4;
  return 6;
}

std::string FormatPrice(double value, int decimals) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
  return std::string(buf);
}

}  // namespace

// ===========================================================================
// UiState
// ===========================================================================

void UiState::PushTick(double price) {
  std::lock_guard<std::mutex> lock(mutex_);
  ticks_.push_back(price);
  while (ticks_.size() > kMaxTicks) ticks_.pop_front();
  ++tick_seq_;
}

void UiState::PushMarker(TradeMarker marker) {
  std::lock_guard<std::mutex> lock(mutex_);
  // anchor the marker to the most recent ticker sample.
  marker.seq = tick_seq_ > 0 ? tick_seq_ - 1 : 0;
  markers_.push_back(marker);
  while (markers_.size() > kMaxMarkers) markers_.pop_front();
}

void UiState::SetSymbol(const std::string& symbol) {
  std::lock_guard<std::mutex> lock(mutex_);
  symbol_ = symbol;
}

UiState::Snapshot UiState::Take() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Snapshot snap;
  snap.ticks = ticks_;
  snap.markers = markers_;
  snap.tick_seq = tick_seq_;
  snap.symbol = symbol_;
  return snap;
}

// ===========================================================================
// TuiDashboard lifecycle
// ===========================================================================

TuiDashboard::TuiDashboard() : TuiDashboard(Options{}) {}

TuiDashboard::TuiDashboard(Options opts) : opts_(std::move(opts)) {}

TuiDashboard::~TuiDashboard() {
  running_.store(false, std::memory_order_release);
  if (redis_thread_.joinable()) {
    redis_thread_.join();
  }
  DisconnectRedis();
}

int TuiDashboard::Run() {
  // file-only logging: console output would corrupt the notcurses surface.
  mokuren::InitLogger("logs/tui_dashboard.log", spdlog::level::info, false);

  notcurses_options nopts{};
  nopts.flags = NCOPTION_SUPPRESS_BANNERS;
  nc_ = notcurses_init(&nopts, stdout);
  if (nc_ == nullptr) {
    std::fprintf(stderr, "tui_dashboard: notcurses_init failed\n");
    return 1;
  }

  Layout();

  running_.store(true, std::memory_order_release);
  redis_thread_ = std::thread(&TuiDashboard::RedisWorker, this);

  const auto frame_budget =
      std::chrono::milliseconds(1000 / std::max(1, opts_.fps));
  auto next_frame = std::chrono::steady_clock::now();

  while (running_.load(std::memory_order_acquire)) {
    // non-blocking input: never stalls the render loop.
    ncinput ni{};
    const char32_t key = notcurses_get_nblock(nc_, &ni);
    if (key != 0) {
      if (key == 'q' || key == 'Q') {
        break;
      }
      if (key == NCKEY_RESIZE) {
        Layout();
      }
    }

    RenderFrame();
    notcurses_render(nc_);

    next_frame += frame_budget;
    const auto now = std::chrono::steady_clock::now();
    if (next_frame > now) {
      std::this_thread::sleep_until(next_frame);
    } else {
      next_frame = now + frame_budget;  // avoid unbounded drift after a stall
    }
  }

  running_.store(false, std::memory_order_release);
  if (redis_thread_.joinable()) {
    redis_thread_.join();
  }

  notcurses_stop(nc_);
  nc_ = nullptr;

  return 0;
}

// ===========================================================================
// redis worker
// ===========================================================================

bool TuiDashboard::WaitConnected(int timeout_ms) {
  auto* ctx = static_cast<redisContext*>(redis_ctx_);
  if (ctx == nullptr) return false;

  struct pollfd pfd{};
  pfd.fd = ctx->fd;
  pfd.events = POLLOUT;
  const int rc = ::poll(&pfd, 1, timeout_ms);
  if (rc <= 0) return false;  // timeout or hard error

  int sock_err = 0;
  socklen_t len = sizeof(sock_err);
  if (::getsockopt(ctx->fd, SOL_SOCKET, SO_ERROR, &sock_err, &len) < 0) {
    return false;
  }
  return sock_err == 0;
}

bool TuiDashboard::ConnectRedis() {
  if (redis_ctx_) return true;

  redisContext* ctx =
      redisConnectNonBlock(opts_.redis_host.c_str(), opts_.redis_port);
  if (ctx == nullptr || ctx->err) {
    if (ctx) {
      LOG_WARN("TuiDashboard: redis connect error: {}", ctx->errstr);
      redisFree(ctx);
    } else {
      LOG_WARN("TuiDashboard: redis allocation error");
    }
    return false;
  }

  redis_ctx_ = ctx;
  if (!WaitConnected(kConnectTimeoutMs)) {
    LOG_WARN("TuiDashboard: redis connect to {}:{} timed out",
             opts_.redis_host, opts_.redis_port);
    DisconnectRedis();
    return false;
  }

  redis_connected_.store(true, std::memory_order_relaxed);
  LOG_INFO("TuiDashboard: connected to redis {}:{}", opts_.redis_host,
           opts_.redis_port);
  return true;
}

void TuiDashboard::DisconnectRedis() {
  if (redis_ctx_) {
    redisFree(static_cast<redisContext*>(redis_ctx_));
    redis_ctx_ = nullptr;
  }
  redis_connected_.store(false, std::memory_order_relaxed);
}

void TuiDashboard::ParseTickerEntry(redisReply* fields) {
  std::string symbol;
  double price = 0.0;
  int64_t ts = 0;

  for (size_t i = 0; i + 1 < fields->elements; i += 2) {
    const char* key = fields->element[i]->str;
    const char* val = fields->element[i + 1]->str;
    if (key == nullptr || val == nullptr) continue;
    if (std::strcmp(key, "symbol") == 0) {
      symbol = val;
    } else if (std::strcmp(key, "price") == 0) {
      price = std::strtod(val, nullptr);
    } else if (std::strcmp(key, "timestamp") == 0) {
      ts = std::strtoll(val, nullptr, 10);
    }
  }

  if (!symbol.empty()) state_.SetSymbol(symbol);
  if (price > 0.0) state_.PushTick(price);
  (void)ts;  // reserved for future time-axis rendering
}

void TuiDashboard::ParseTradeEntry(redisReply* fields) {
  const char* payload = nullptr;
  for (size_t i = 0; i + 1 < fields->elements; i += 2) {
    if (fields->element[i]->str != nullptr &&
        std::strcmp(fields->element[i]->str, "payload") == 0) {
      payload = fields->element[i + 1]->str;
      break;
    }
  }
  if (payload == nullptr) return;

  const nlohmann::json j = nlohmann::json::parse(payload, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return;

  const double price =
      JsonNumber(j, {"price", "entry_price", "fill_price", "exit_price"}, 0.0);
  if (price <= 0.0) return;

  TradeMarker marker;
  marker.price = price;
  marker.timestamp_ms =
      JsonInt(j, {"timestamp", "time", "ts", "entry_time", "exit_time"}, 0);
  marker.side =
      SideFromString(JsonString(j, {"direction", "side", "action"}));
  state_.PushMarker(marker);
}

void TuiDashboard::ProcessReply(redisReply* reply) {
  // XREAD reply shape:
  //   * streams
  //     * 2
  //       name
  //       * entries
  //         * 2 [ id, * fields ]
  if (reply->type != REDIS_REPLY_ARRAY) return;

  for (size_t s = 0; s < reply->elements; ++s) {
    redisReply* stream = reply->element[s];
    if (stream->type != REDIS_REPLY_ARRAY || stream->elements < 2) continue;

    const std::string name =
        stream->element[0]->str ? stream->element[0]->str : "";
    redisReply* entries = stream->element[1];
    if (entries->type != REDIS_REPLY_ARRAY) continue;

    std::string last_id;
    for (size_t e = 0; e < entries->elements; ++e) {
      redisReply* entry = entries->element[e];
      if (entry->type != REDIS_REPLY_ARRAY || entry->elements < 2) continue;

      last_id = entry->element[0]->str ? entry->element[0]->str : "";
      redisReply* fields = entry->element[1];
      if (fields->type != REDIS_REPLY_ARRAY) continue;

      if (name == RedisPublisher::kTickerStream) {
        ParseTickerEntry(fields);
      } else if (name == RedisPublisher::kTradeStream) {
        ParseTradeEntry(fields);
      }
    }

    // advance the cursor so a reconnect resumes right after the last entry.
    if (!last_id.empty()) {
      if (name == RedisPublisher::kTickerStream) {
        next_id_ticker_ = last_id;
      } else if (name == RedisPublisher::kTradeStream) {
        next_id_trade_ = last_id;
      }
    }
  }
}

void TuiDashboard::RedisWorker() {
  LOG_INFO("TuiDashboard: redis worker online");

  while (running_.load(std::memory_order_acquire)) {
    if (!ConnectRedis()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectDelayMs));
      continue;
    }

    auto* ctx = static_cast<redisContext*>(redis_ctx_);

    // the exact blocking read requested; ids are "$" on the first pass.
    if (redisAppendCommand(ctx, "XREAD BLOCK 0 STREAMS %s %s %s %s",
                           RedisPublisher::kTickerStream,
                           RedisPublisher::kTradeStream,
                           next_id_ticker_.c_str(),
                           next_id_trade_.c_str()) != REDIS_OK) {
      DisconnectRedis();
      continue;
    }

    bool got_reply = false;
    bool connection_ok = true;

    // pump the non-blocking socket until one reply lands or we lose the
    // connection. poll() lets us observe shutdown within kPollTimeoutMs.
    while (running_.load(std::memory_order_acquire)) {
      int write_done = 0;
      if (redisBufferWrite(ctx, &write_done) == REDIS_ERR) {
        connection_ok = false;
        break;
      }
      if (!write_done) {
        struct pollfd pfd{};
        pfd.fd = ctx->fd;
        pfd.events = POLLOUT;
        ::poll(&pfd, 1, kPollTimeoutMs);
        continue;
      }

      if (redisBufferRead(ctx) == REDIS_ERR) {
        connection_ok = false;
        break;
      }

      void* raw = nullptr;
      if (redisReaderGetReply(ctx->reader, &raw) == REDIS_ERR) {
        connection_ok = false;
        break;
      }
      if (raw != nullptr) {
        ProcessReply(static_cast<redisReply*>(raw));
        freeReplyObject(raw);
        got_reply = true;
        break;
      }

      struct pollfd pfd{};
      pfd.fd = ctx->fd;
      pfd.events = POLLIN;
      const int prc = ::poll(&pfd, 1, kPollTimeoutMs);
      if (prc < 0 && errno != EINTR) {
        connection_ok = false;
        break;
      }
    }

    if (!got_reply && !connection_ok) {
      LOG_WARN("TuiDashboard: redis read failed, reconnecting");
      DisconnectRedis();
    }
  }

  DisconnectRedis();
  LOG_INFO("TuiDashboard: redis worker offline");
}

// ===========================================================================
// notcurses layout + rendering
// ===========================================================================

void TuiDashboard::Layout() {
  // the price chart is drawn from scratch onto the standard plane every
  // frame, so a resize needs no precomputed geometry. kept as the resize
  // hook for future layout bookkeeping.
}

void TuiDashboard::RenderPriceChart(ncplane* plane,
                                    const UiState::Snapshot& snap,
                                    const ChartGeometry& geo) {
  if (geo.w < 4 || geo.h < 2 || snap.ticks.empty()) return;

  // --- y domain, padded so the line never touches the chart frame ------
  double miny = snap.ticks.front();
  double maxy = snap.ticks.front();
  for (double v : snap.ticks) {
    miny = std::min(miny, v);
    maxy = std::max(maxy, v);
  }
  if (maxy - miny <= 0.0) {
    // flat market: synthesise a symmetric band around the last price.
    const double pad = std::max(1.0, std::abs(maxy) * 0.0005);
    miny -= pad;
    maxy += pad;
  } else {
    const double pad = (maxy - miny) * 0.05;
    miny -= pad;
    maxy += pad;
  }
  const double range = std::max(maxy - miny, 1e-9);

  const int pixel_w = geo.w * 2;  // braille: 2 subpixels per cell column
  const int pixel_h = geo.h * 4;  // and 4 per cell row
  const int64_t n = static_cast<int64_t>(snap.ticks.size());
  const int64_t base_seq = snap.tick_seq - n;

  std::vector<std::vector<uint8_t>> line(
      static_cast<size_t>(geo.w),
      std::vector<uint8_t>(static_cast<size_t>(geo.h), 0));
  std::vector<std::vector<uint8_t>> refs(
      static_cast<size_t>(geo.w),
      std::vector<uint8_t>(static_cast<size_t>(geo.h), 0));

  auto set_pixel = [&](int px, int py) {
    if (px < 0 || px >= pixel_w || py < 0 || py >= pixel_h) return;
    line[static_cast<size_t>(px / 2)][static_cast<size_t>(py / 4)] |=
        kBrailleDots[px % 2][py % 4];
  };

  // bresenham so consecutive samples form a continuous line, not dots.
  auto plot_segment = [&](int x0, int y0, int x1, int y1) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
      set_pixel(x0, y0);
      if (x0 == x1 && y0 == y1) break;
      const int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  };

  int prev_px = 0;
  int prev_py = 0;
  for (int64_t i = 0; i < n; ++i) {
    const double nx =
        (n <= 1) ? 0.5 : static_cast<double>(i) / static_cast<double>(n - 1);
    const double ny = (snap.ticks[static_cast<size_t>(i)] - miny) / range;
    // screen y grows downward, so invert the normalised price.
    const int px = static_cast<int>(std::lround(nx * (pixel_w - 1)));
    const int py = static_cast<int>(std::lround((1.0 - ny) * (pixel_h - 1)));
    if (i == 0) {
      set_pixel(px, py);
    } else {
      plot_segment(prev_px, prev_py, px, py);
    }
    prev_px = px;
    prev_py = py;
  }

  // --- reference gridlines + y-axis price labels -----------------------
  constexpr int kRefLevels = 5;
  const int decimals = PriceDecimals(range);

  int last_label_row = -1;
  for (int g = 0; g < kRefLevels; ++g) {
    const double value =
        miny + range * static_cast<double>(g) /
                   static_cast<double>(kRefLevels - 1);
    const double ny = (value - miny) / range;
    const int py = static_cast<int>(std::lround((1.0 - ny) * (pixel_h - 1)));
    if (py < 0 || py >= pixel_h) continue;

    const int row = py / 4;
    for (int cx = 0; cx < geo.w; ++cx) {
      refs[static_cast<size_t>(cx)][static_cast<size_t>(row)] |=
          kBrailleDots[0][py % 4];
    }

    const int screen_row = geo.y + row;
    if (screen_row == last_label_row) continue;  // avoid overlapping labels
    last_label_row = screen_row;

    ncplane_set_bg_default(plane);
    ncplane_set_fg_rgb8(plane, 120, 120, 120);
    const std::string label = FormatPrice(value, decimals);
    int col = geo.x - 2 - static_cast<int>(label.size());
    if (col < 0) col = 0;
    ncplane_putstr_yx(plane, screen_row, col, label.c_str());
    ncplane_putstr_yx(plane, screen_row, geo.x - 1, "-");
  }

  // --- blit braille cells (price line wins over the dim gridline) ------
  for (int cy = 0; cy < geo.h; ++cy) {
    for (int cx = 0; cx < geo.w; ++cx) {
      const uint8_t lv = line[static_cast<size_t>(cx)][static_cast<size_t>(cy)];
      const uint8_t rv = refs[static_cast<size_t>(cx)][static_cast<size_t>(cy)];
      uint8_t bits = 0;
      if (lv != 0) {
        bits = lv;
        ncplane_set_fg_rgb8(plane, 0, 200, 255);
      } else if (rv != 0) {
        bits = rv;
        ncplane_set_fg_rgb8(plane, 70, 70, 70);
      } else {
        continue;
      }

      const int code = 0x2800 + bits;
      char utf8[4];
      utf8[0] = static_cast<char>(0xE0 | (code >> 12));
      utf8[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      utf8[2] = static_cast<char>(0x80 | (code & 0x3F));
      utf8[3] = '\0';
      ncplane_putstr_yx(plane, geo.y + cy, geo.x + cx, utf8);
    }
  }

  // --- x-axis (time) captions ------------------------------------------
  unsigned rows_all = 0;
  unsigned cols_all = 0;
  ncplane_dim_yx(plane, &rows_all, &cols_all);
  const int axis_row = geo.y + geo.h;
  if (axis_row < static_cast<int>(rows_all)) {
    ncplane_set_bg_default(plane);
    ncplane_set_fg_rgb8(plane, 120, 120, 120);

    // samples are pushed at a steady cadence, so the index doubles as time.
    const std::string left = "t-" + std::to_string(n > 1 ? n - 1 : 0);
    ncplane_putstr_yx(plane, axis_row, geo.x, left.c_str());

    const std::string mid = "t-" + std::to_string(n > 1 ? (n - 1) / 2 : 0);
    int mid_col = geo.x + geo.w / 2 - static_cast<int>(mid.size()) / 2;
    if (mid_col < geo.x) mid_col = geo.x;
    ncplane_putstr_yx(plane, axis_row, mid_col, mid.c_str());

    const std::string right = "now";
    int right_col = geo.x + geo.w - static_cast<int>(right.size());
    if (right_col + static_cast<int>(right.size()) >
        static_cast<int>(cols_all)) {
      right_col = static_cast<int>(cols_all) - static_cast<int>(right.size());
    }
    if (right_col < geo.x) right_col = geo.x;
    ncplane_putstr_yx(plane, axis_row, right_col, right.c_str());
  }

  DrawMarkers(plane, snap, geo, base_seq, miny, maxy);
}

void TuiDashboard::DrawMarkers(ncplane* plane, const UiState::Snapshot& snap,
                               const ChartGeometry& geo, int64_t base_seq,
                               double miny, double maxy) {
  const int64_t n = static_cast<int64_t>(snap.ticks.size());
  if (n == 0 || maxy <= miny || geo.w <= 0 || geo.h <= 0) return;

  unsigned uh = 0;
  unsigned uw = 0;
  ncplane_dim_yx(plane, &uh, &uw);
  if (uh == 0 || uw == 0) return;

  const int pixel_w = geo.w * 2;
  const int pixel_h = geo.h * 4;
  const double yrange = maxy - miny;
  const int64_t window_right = snap.tick_seq - 1;

  for (const TradeMarker& marker : snap.markers) {
    if (marker.seq < base_seq || marker.seq > window_right) continue;

    const double nx =
        (n <= 1) ? 0.5
                 : static_cast<double>(marker.seq - base_seq) /
                       static_cast<double>(n - 1);
    double ny = (marker.price - miny) / yrange;
    ny = std::clamp(ny, 0.0, 1.0);

    // map onto the very same braille subpixel grid the price line uses.
    const int px = static_cast<int>(std::lround(nx * (pixel_w - 1)));
    const int py = static_cast<int>(std::lround((1.0 - ny) * (pixel_h - 1)));
    const int cell_x = geo.x + px / 2;
    const int cell_y = geo.y + py / 4;
    if (cell_x < 0 || cell_x >= static_cast<int>(uw) || cell_y < 0 ||
        cell_y >= static_cast<int>(uh)) {
      continue;
    }

    switch (marker.side) {
      case MarkerSide::Buy:
        ncplane_set_fg_rgb8(plane, 0, 0, 0);
        ncplane_set_bg_rgb8(plane, 40, 230, 110);
        break;
      case MarkerSide::Sell:
        ncplane_set_fg_rgb8(plane, 0, 0, 0);
        ncplane_set_bg_rgb8(plane, 240, 50, 50);
        break;
      default:
        ncplane_set_fg_rgb8(plane, 0, 0, 0);
        ncplane_set_bg_rgb8(plane, 240, 200, 40);
        break;
    }

    // preserve the glyph (braille or blank), just recolour the cell.
    uint16_t attr = 0;
    uint64_t channels = 0;
    char* under = ncplane_at_yx(plane, cell_y, cell_x, &attr, &channels);
    if (under != nullptr) {
      ncplane_putstr_yx(plane, cell_y, cell_x, under);
      free(under);
    } else {
      ncplane_putchar_yx(plane, cell_y, cell_x, ' ');
    }
  }
}

void TuiDashboard::RenderFrame() {
  ncplane* std_plane = notcurses_stdplane(nc_);
  unsigned rows = 0;
  unsigned cols = 0;
  ncplane_dim_yx(std_plane, &rows, &cols);

  ncplane_erase(std_plane);

  const UiState::Snapshot snap = state_.Take();

  // --- header ---------------------------------------------------------
  ncplane_set_fg_rgb8(std_plane, 0, 200, 255);
  ncplane_set_bg_default(std_plane);
  ncplane_putstr_yx(std_plane, 0, 0, "MOKUREN // LIVE MARKET");
  ncplane_set_fg_rgb8(std_plane, 120, 120, 120);
  if (cols > 18) {
    ncplane_putstr_yx(std_plane, 0, static_cast<int>(cols) - 14, "[q] quit");
  }

  ncplane_set_fg_rgb8(std_plane, 230, 230, 230);
  ncplane_printf_yx(std_plane, 1, 0,
                    "symbol %-10s  last %12.2f  ticks %3zu  markers %3zu",
                    snap.symbol.c_str(),
                    snap.ticks.empty() ? 0.0 : snap.ticks.back(),
                    snap.ticks.size(), snap.markers.size());

  // --- plot -----------------------------------------------------------
  if (!snap.ticks.empty()) {
    // 13-column gutter for the y-axis price labels, plus a margin on the
    // right and one row below the plot for the time captions.
    constexpr int kAxisGutter = 13;
    ChartGeometry geo;
    geo.x = kAxisGutter;
    geo.y = 3;
    geo.w = static_cast<int>(cols) - kAxisGutter - 2;
    geo.h = static_cast<int>(rows) - geo.y - 3;
    if (geo.w >= 4 && geo.h >= 2) {
      RenderPriceChart(std_plane, snap, geo);
    }
  } else {
    ncplane_set_fg_rgb8(std_plane, 120, 120, 120);
    ncplane_putstr_yx(std_plane, 3, 0, "waiting for ticker data...");
  }

  // --- footer ---------------------------------------------------------
  const bool connected = redis_connected_.load(std::memory_order_relaxed);
  // markers paint their own background; reset it before the footer text.
  ncplane_set_bg_default(std_plane);
  if (connected) {
    ncplane_set_fg_rgb8(std_plane, 80, 220, 120);
  } else {
    ncplane_set_fg_rgb8(std_plane, 240, 160, 40);
  }
  ncplane_printf_yx(std_plane, static_cast<int>(rows) - 1, 0,
                    "redis %s  %s:%d  |  streams %s / %s",
                    connected ? "connected" : "reconnecting",
                    opts_.redis_host.c_str(), opts_.redis_port,
                    RedisPublisher::kTickerStream,
                    RedisPublisher::kTradeStream);
}

}  // namespace mokuren::tui
