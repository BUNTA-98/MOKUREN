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
#include "recorder.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

// Set from a signal handler, so it is the only thing that may be written there.
std::atomic<bool> g_stop{false};

static_assert(std::atomic<bool>::is_always_lock_free,
              "the signal handler stores to this atomic: a handler that could "
              "take a lock can deadlock the process it was meant to stop");

struct Args {
  std::string symbol = "BTCUSDT";
  std::string out;              // empty -> generated under recordings/
  std::string venue = "bybit";
  std::string env = "mainnet";
  std::string category = "linear";
  int64_t duration_s = 0;       // 0 = until interrupted
  int64_t l2_min_interval_ms = 0;
  bool validate = true;
};

void PrintUsage() {
  std::cout <<
      "usage: mokuren_record [options]\n"
      "\n"
      "  --symbol NAME        venue symbol (default BTCUSDT)\n"
      "  --out PATH           output .mkn file\n"
      "                       (default recordings/<venue>_<symbol>_<utc>.mkn)\n"
      "  --venue NAME         session metadata only (default bybit)\n"
      "  --env NAME           session metadata only (default mainnet)\n"
      "  --category NAME      session metadata only (default linear)\n"
      "  --duration SECONDS   stop after N seconds (0 = until ctrl-c)\n"
      "  --l2-interval MS     at most one book frame per MS (0 = every frame)\n"
      "  --no-validate        write frames the live gate would reject\n"
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
    } else if (arg == "--no-validate") {
      a.validate = false;
    } else {
      err = "unknown option '" + arg + "'";
      return false;
    }
  }
  return true;
}

std::string DefaultPath(const Args& a) {
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

void PrintStats(const mokuren::MarketDataRecorder& rec,
                const std::string& path) {
  const auto& s = rec.stats();
  std::cout << "\n--- recording summary ---\n"
            << "file:              " << path << "\n"
            << "trades  seen:      " << s.trades_seen << "\n"
            << "trades  written:   " << s.trades_written << "\n"
            << "trades  rejected:  " << s.trades_rejected << "\n"
            << "l2      seen:      " << s.l2_seen << "\n"
            << "l2      written:   " << s.l2_written << "\n"
            << "l2      rejected:  " << s.l2_rejected << "\n"
            << "l2      throttled: " << s.l2_throttled << "\n"
            << "l2      gaps:      " << s.l2_gaps << "\n";

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

  // Only bybit has a streamer today. Refusing an unknown venue is better than
  // recording a bybit feed into a file labelled "binance": the metadata is what
  // the reader uses to pick a profile, so a wrong label is a wrong book later.
  if (args.venue != "bybit") {
    std::cerr << "error: no streamer for venue '" << args.venue
              << "' (only bybit is wired)\n";
    return EXIT_FAILURE;
  }

  const std::string path = args.out.empty() ? DefaultPath(args) : args.out;

  // fopen() does not create the directory, and "cannot open recordings/x.mkn"
  // is a worse error than the one that actually happened.
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      std::cerr << "error: cannot create '" << parent.string()
                << "': " << ec.message() << "\n";
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
  // The recorder has no strategy config, so it enforces structure only: one
  // level per side is the minimum a frame must carry to be a book at all. What
  // the triggers need on top of that is a strategy decision, and the live gate
  // already refuses to arm below it.
  cfg.min_book_depth = 1;

  mokuren::MarketDataRecorder recorder(cfg);
  BybitDataStreamer streamer(args.symbol);

  // The recorder takes the data callbacks; the lifecycle callbacks stay here,
  // because this process owns the stream. A recorder that decided when a
  // session ends would produce a recording with a hole in it that nobody was
  // told about.
  streamer.SetFinishedCallback([&] {
    std::cout << "\nfeed reported natural end of input\n";
    g_stop.store(true, std::memory_order_relaxed);
  });
  streamer.SetErrorCallback([&](const std::string& message) {
    std::cerr << "\nfeed error: " << message << "\n";
    g_stop.store(true, std::memory_order_relaxed);
  });

  recorder.Attach(streamer);

  if (!recorder.Open()) {
    std::cerr << "error: cannot open '" << path
              << "': " << recorder.last_error() << "\n";
    return EXIT_FAILURE;
  }

  std::signal(SIGINT, MokurenRecordOnSignal);
  std::signal(SIGTERM, MokurenRecordOnSignal);

  std::cout << "recording " << args.venue << " " << args.symbol << " -> "
            << path << "\n"
            << "l2_interval_ms=" << args.l2_min_interval_ms
            << " validate=" << (args.validate ? "on" : "off")
            << "\npress ctrl-c to stop\n";

  streamer.Start();

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
  streamer.Stop();
  const bool closed_ok = recorder.Close();

  PrintStats(recorder, path);

  if (!closed_ok) {
    std::cerr << "error: the file could not be finalised and is truncated\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
