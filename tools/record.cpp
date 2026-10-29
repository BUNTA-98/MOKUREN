// Records one venue's public feed to a .mkn market-data file.
//
// Deliberately a separate binary from `mokuren`. Recording is not trading: it
// never arms the aggregator, sizes a position or touches the broker. The one
// thing it shares with the engine is the parser that already runs live -- which
// is the point. A recording is only as trustworthy as the code that produced
// it, so this must not own a second websocket client, a second book rebuild or
// a second set of parser rules.
//
// Usage:
//   mokuren_record --symbol BTCUSDT --out recordings/btc.mkn --duration 60
//   mokuren_record --symbol ETHUSDT --l2-interval 100
//   mokuren_record --help

#include "bybit_streamer.hpp"
#include "hyperliquid_streamer.hpp"
#include "live_debug.hpp"
#include "logger.hpp"
#include "recorder.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

// Set from a signal handler, so it is the only thing that may be written there.
std::atomic<bool> g_stop{false};

static_assert(std::atomic<bool>::is_always_lock_free,
              "the signal handler stores to this atomic: a handler that could "
              "take a lock can deadlock the process it was meant to stop");

struct Args {
  // Empty means "use the venue's default", resolved by main() once --venue is
  // known. Seeding these with bybit's values here would make "--venue
  // hyperliquid" subscribe to BTCUSDT, which hyperliquid does not list: the
  // socket stays open, nothing ever arrives, and an empty recording is
  // indistinguishable from a quiet market.
  std::string symbol;
  std::string out;              // empty -> generated under recordings/
  std::string venue = "bybit";
  std::string env = "mainnet";
  std::string category;
  int64_t duration_s = 0;       // 0 = until interrupted
  int64_t l2_min_interval_ms = 0;
  bool validate = true;
  bool debug = false;

  // Daily mode: `out` names a root directory, the recorder writes one file per
  // UTC day beneath <root>/<venue>-<symbol>/.
  bool daily = false;
  // Daily mode only: overwrite a same-day restart's file instead of writing a
  // T<HHMMSS> sibling.
  bool overwrite_existing = false;
};

void PrintUsage() {
  std::cout <<
      "usage: mokuren_record [options]\n"
      "\n"
      "  --symbol NAME        venue symbol\n"
      "                       (default: BTCUSDT on bybit, BTC on hyperliquid)\n"
      "  --out PATH           output .mkn file; with --daily, the root\n"
      "                       directory to write day files under\n"
      "                       (default recordings/<venue>_<symbol>_<utc>.mkn,\n"
      "                        or recordings/ with --daily)\n"
      "  --venue NAME         which streamer to record (default bybit)\n"
      "                       supported: bybit, hyperliquid\n"
      "  --env NAME           session metadata only (default mainnet)\n"
      "  --category NAME      session metadata only (default linear)\n"
      "                       one of: linear, spot, inverse\n"
      "  --duration SECONDS   stop after N seconds (0 = until ctrl-c)\n"
      "  --l2-interval MS     at most one book frame per MS (0 = every frame)\n"
      "  --daily              --out is a root directory: one file per UTC day,\n"
      "                       rotated at midnight, never mid-record\n"
      "  --overwrite-existing with --daily: replace a same-day restart's file\n"
      "                       instead of writing a T<HHMMSS> sibling\n"
      "  --no-validate        write frames the live gate would reject\n"
      "  --debug              log every frame the parser sees, and why it was\n"
      "                       dropped; the only way to tell a slow venue from a\n"
      "                       frame that never reached the recorder\n"
      "  -h, --help           this text\n"
      "\n"
      "Trades are never downsampled: a missing print changes the footprint,\n"
      "a coarser book mostly does not.\n";
}

// Every value is a separate argv, which keeps the parser honest about missing
// values instead of silently reading "".
bool NeedsValue(int argc, int i, const char* flag, std::string& err) {
  if (i + 1 >= argc) {
    err = std::string(flag) + " requires a value";
    return false;
  }
  return true;
}

bool ParseArgs(int argc, char** argv, Args& a, bool& wants_help,
               std::string& err) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      wants_help = true;
      return true;
    }

    auto next = [&](const char* flag, std::string& out) -> bool {
      if (!NeedsValue(argc, i, flag, err)) return false;
      out = argv[++i];
      return true;
    };

    if (arg == "--symbol") {
      if (!next("--symbol", a.symbol)) return false;
    } else if (arg == "--out") {
      if (!next("--out", a.out)) return false;
    } else if (arg == "--venue") {
      if (!next("--venue", a.venue)) return false;
    } else if (arg == "--env") {
      if (!next("--env", a.env)) return false;
    } else if (arg == "--category") {
      if (!next("--category", a.category)) return false;
    } else if (arg == "--duration") {
      std::string v;
      if (!next("--duration", v)) return false;
      try {
        a.duration_s = std::stoll(v);
      } catch (const std::exception&) {
        err = "--duration must be an integer number of seconds";
        return false;
      }
      if (a.duration_s < 0) {
        err = "--duration must not be negative";
        return false;
      }
    } else if (arg == "--l2-interval") {
      std::string v;
      if (!next("--l2-interval", v)) return false;
      try {
        a.l2_min_interval_ms = std::stoll(v);
      } catch (const std::exception&) {
        err = "--l2-interval must be an integer number of milliseconds";
        return false;
      }
      if (a.l2_min_interval_ms < 0) {
        err = "--l2-interval must not be negative";
        return false;
      }
    } else if (arg == "--debug") {
      a.debug = true;
    } else if (arg == "--daily") {
      a.daily = true;
    } else if (arg == "--overwrite-existing") {
      a.overwrite_existing = true;
    } else if (arg == "--no-validate") {
      a.validate = false;
    } else {
      err = "unknown option '" + arg + "'";
      return false;
    }
  }
  return true;
}

// Builds the streamer for the requested venue.
//
// Returning nullptr rather than falling back to bybit is deliberate: a bybit
// feed written into a file labelled "hyperliquid" is read back with the wrong
// profile and rebuilds the wrong book, and nothing in the file says so.
std::unique_ptr<IMarketStreamer> MakeStreamer(const Args& a, std::string& err) {
  if (a.venue == "bybit") {
    return std::make_unique<BybitDataStreamer>(a.symbol);
  }
  if (a.venue == "hyperliquid") {
    return std::make_unique<HyperliquidDataStreamer>(a.symbol);
  }
  err = "no streamer for venue '" + a.venue +
        "' (supported: bybit, hyperliquid)";
  return nullptr;
}

std::string DefaultPath(const Args& a) {
  // Daily mode needs a root directory, not a file: the recorder appends
  // <venue>-<symbol>/ and the per-day filename itself. Handing it the
  // single-file default would create a directory literally named
  // "recordings/bybit_BTCUSDT_<stamp>.mkn".
  if (a.daily) return "recordings";

  std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &tm);

  // One directory under the working tree, so a session is a file that can be
  // moved, not a name that depends on where the binary was started from.
  return "recordings/" + a.venue + "_" + a.symbol + "_" + stamp + ".mkn";
}

void PrintStats(const mokuren::MarketDataRecorder& rec, const std::string& path,
                int64_t elapsed_s, bool daily) {
  const auto& s = rec.stats();
  std::cout << "\n--- recording summary ---\n"
            << (daily ? "root:              " : "file:              ") << path
            << "\n"
            << "elapsed:           " << elapsed_s << "s\n"
            << "trades  seen:      " << s.trades_seen << "\n"
            << "trades  written:   " << s.trades_written << "\n"
            << "trades  rejected:  " << s.trades_rejected << "\n"
            << "l2      seen:      " << s.l2_seen << "\n"
            << "l2      written:   " << s.l2_written << "\n"
            << "l2      rejected:  " << s.l2_rejected << "\n"
            << "l2      throttled: " << s.l2_throttled << "\n"
            << "l2      gaps:      " << s.l2_gaps << "\n";
  // Only meaningful with --daily; a silent zero everywhere else is noise.
  if (daily) {
    std::cout << "rotations:         " << s.rotations << "\n";
  }

  // Per minute, because that is the unit StartupGate states its criteria in: a
  // session and a probe then compare without conversion. Without this line a
  // count is unreadable -- 248 trades is a healthy session at 30s and a starved
  // one at 30min, and nothing above says which.
  if (elapsed_s > 0) {
    std::cout << "trades  /min:      " << s.trades_written * 60 / elapsed_s
              << "\n"
              << "l2      /min:      " << s.l2_written * 60 / elapsed_s << "\n";
  }

  if (!rec.last_error().empty()) {
    std::cerr << "writer error: " << rec.last_error() << "\n";
  }
  if (s.trades_written == 0 && s.l2_written == 0) {
    std::cerr << "nothing was written: the feed produced no usable frames "
                 "(check the symbol and the venue status)\n";
  }
}

}  // namespace

extern "C" void MokurenRecordOnSignal(int) {
  // async-signal-safe: one relaxed store, no allocation, no logging.
  g_stop.store(true, std::memory_order_relaxed);
}

int main(int argc, char** argv) {
  Args args;
  bool wants_help = false;
  std::string err;

  if (!ParseArgs(argc, argv, args, wants_help, err)) {
    std::cerr << "error: " << err << "\n\n";
    PrintUsage();
    return EXIT_FAILURE;
  }
  if (wants_help) {
    PrintUsage();
    return EXIT_SUCCESS;
  }

  // The streamers log through the global logger, which is a null sink until it
  // is initialised: without this call every LOG_* and LIVE_DBG in the parser is
  // silently discarded, including the ones --debug exists to enable. A separate
  // file from logs/engine.log on purpose -- two processes rotating one file
  // interleave each other's lines.
  const auto log_level =
      args.debug ? spdlog::level::trace : spdlog::level::warn;
  mokuren::InitLogger("logs/record.log", log_level,
                      /*enable_console=*/args.debug);
  spdlog::set_level(log_level);

  if (args.debug) {
    // Set before the streamer is built, so the constructor's own tracing is
    // included. The parser already logs one line per raw frame and one per
    // parsed event; without this flag those lines are unreachable from the
    // binary that produces the recording, and a frame that arrived and yielded
    // nothing is indistinguishable from a venue that never sent it.
    mokuren::debug::g_live_debug_enabled.store(true, std::memory_order_relaxed);
  }

  // The symbol is genuinely per-venue ("BTC" vs "BTCUSDT") and only resolvable
  // now: subscribing to the wrong coin leaves the socket open and silent, which
  // is indistinguishable from a quiet market.
  if (args.symbol.empty()) {
    args.symbol = (args.venue == "hyperliquid") ? "BTC" : "BTCUSDT";
  }
  // The category is NOT per-venue, despite looking like it should be. It is a
  // closed set of contract kinds -- linear, spot, inverse -- written into the
  // session metadata and consumed by venue::Resolve. "perp" is not in that set:
  // defaulting to it for hyperliquid produced a file whose metadata named a
  // category no reader accepts, and made every config that copied the value
  // fail AppConfig validation. Hyperliquid perps are USDC-margined linear
  // contracts, which is the closest truthful mapping and what every reader
  // already understands.
  if (args.category.empty()) {
    args.category = "linear";
  }

  // Built before the file is touched: an unknown venue should fail before this
  // process creates a directory and an empty .mkn behind it.
  std::string streamer_err;
  std::unique_ptr<IMarketStreamer> streamer = MakeStreamer(args, streamer_err);
  if (!streamer) {
    std::cerr << "error: " << streamer_err << "\n";
    mokuren::ShutdownLogger();
    return EXIT_FAILURE;
  }

  const std::string path = args.out.empty() ? DefaultPath(args) : args.out;

  // --daily reinterprets --out, so a typo'd single-file path (or a stale
  // recording from an earlier run) must fail loudly instead of being adopted as
  // a root directory and writing the day files beside it.
  if (args.daily) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
      std::cerr << "error: --daily treats --out as a root directory, but '"
                << path << "' is a file\n";
      mokuren::ShutdownLogger();
      return EXIT_FAILURE;
    }
  }

  if (args.debug) {
    // The frame trace is its own logger with its own sink, not the async audit
    // logger: it is written per frame and would drown the events that matter.
    // Putting it under the recording's stem is what makes the two files a pair
    // -- the default is a fixed name in the working directory, truncated on
    // first use, so a second run would destroy the first run's trace.
    std::string trace;
    if (args.daily) {
      // `path` is a directory here. Naming the trace after it would make every
      // daily session under that root truncate one shared file, so give each
      // instrument its own trace next to the day files it describes.
      trace = (std::filesystem::path(path) / (args.venue + "-" + args.symbol))
                  .string();
    } else {
      trace = path;
      constexpr const char* kExt = ".mkn";
      const std::string ext(kExt);
      if (trace.size() >= ext.size() &&
          trace.compare(trace.size() - ext.size(), ext.size(), ext) == 0) {
        trace.resize(trace.size() - ext.size());
      }
    }
    // spdlog creates the file, not the directories above it.
    const std::filesystem::path trace_parent =
        std::filesystem::path(trace).parent_path();
    if (!trace_parent.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(trace_parent, ec);
    }
    mokuren::debug::LiveDebugLogPath() = trace + ".debug.log";
  }

  // fopen() does not create the directory, and "cannot open recordings/x.mkn"
  // is a worse error than the one that actually happened.
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      std::cerr << "error: cannot create '" << parent.string()
                << "': " << ec.message() << "\n";
      mokuren::ShutdownLogger();
      return EXIT_FAILURE;
    }
  }

  mokuren::RecorderConfig cfg;
  cfg.path = path;
  cfg.venue = args.venue;
  cfg.env = args.env;
  cfg.category = args.category;
  cfg.symbol = args.symbol;
  cfg.l2_min_interval_ms = args.l2_min_interval_ms;
  cfg.validate = args.validate;
  cfg.daily = args.daily;
  // Only consulted by daily mode; harmless otherwise.
  cfg.overwrite_existing = args.overwrite_existing;
  // The recorder has no strategy config, so it enforces structure only: one
  // level per side is the minimum a frame must carry to be a book at all. What
  // the triggers need on top of that is a strategy decision, and the live gate
  // already refuses to arm below it.
  cfg.min_book_depth = 1;

  mokuren::MarketDataRecorder recorder(cfg);

  // The recorder takes the data callbacks; the lifecycle callbacks stay here,
  // because this process owns the stream. A recorder that decided when a
  // session ends would produce a recording with a hole in it that nobody was
  // told about.
  streamer->SetFinishedCallback([&] {
    std::cout << "\nfeed reported natural end of input\n";
    g_stop.store(true, std::memory_order_relaxed);
  });
  streamer->SetErrorCallback([&](const std::string& message) {
    std::cerr << "\nfeed error: " << message << "\n";
    g_stop.store(true, std::memory_order_relaxed);
  });

  recorder.Attach(*streamer);

  if (!recorder.Open()) {
    std::cerr << "error: cannot " << (args.daily ? "prepare" : "open") << " '"
              << path << "': " << recorder.last_error() << "\n";
    mokuren::ShutdownLogger();
    return EXIT_FAILURE;
  }

  std::signal(SIGINT, MokurenRecordOnSignal);
  std::signal(SIGTERM, MokurenRecordOnSignal);

  std::cout << "recording " << args.venue << " " << args.symbol << " -> "
            << path
            << (args.daily ? "/<venue>-<symbol>/<venue>-<symbol>-<UTC date>.mkn"
                           : "")
            << "\n"
            << "l2_interval_ms=" << args.l2_min_interval_ms
            << " validate=" << (args.validate ? "on" : "off")
            << "\npress ctrl-c to stop\n";

  streamer->Start();

  // Poll rather than block on a condition variable: the stop flag is set from a
  // signal handler, which cannot notify one. 100 ms is short enough that ctrl-c
  // feels immediate and long enough not to spin a core.
  const auto started = std::chrono::steady_clock::now();
  auto last_note = started;
  while (!g_stop.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto now = std::chrono::steady_clock::now();
    if (args.duration_s > 0 &&
        now - started >= std::chrono::seconds(args.duration_s)) {
      std::cout << "\nduration reached\n";
      break;
    }
    if (now - last_note >= std::chrono::seconds(30)) {
      last_note = now;
      std::cout << "still recording ("
                << std::chrono::duration_cast<std::chrono::seconds>(now -
                                                                    started)
                       .count()
                << "s)\n";
    }
  }

  // Order matters and is the writer's whole threading contract: Stop() joins
  // the streamer thread, so no callback is in flight when Close() finalises the
  // file. Closing first would race the producer against the header patch.
  streamer->Stop();
  const bool closed_ok = recorder.Close();

  const int64_t elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
  PrintStats(recorder, path, elapsed_s, args.daily);

  // flush the async pool before the process exits, or the last lines -- which
  // are exactly the ones describing why a run was short -- are lost.
  mokuren::ShutdownLogger();

  if (!closed_ok) {
    std::cerr << "error: the file could not be finalised and is truncated\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
