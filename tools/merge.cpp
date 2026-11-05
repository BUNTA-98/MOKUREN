// Small CLI entry for the merger. Kept separate from the library so the
// recorder binary does not link it.
//
//   mokuren_merge --input <dir> --output <root> --venue <v> --symbol <s>
//                 [--kind weekly|monthly] [--overwrite]

#include "market_data/merger.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace {

void Usage() {
  std::fprintf(stderr,
               "usage: mokuren_merge --input DIR --output ROOT "
               "--venue V --symbol S [--kind weekly|monthly|all] "
               "[--overwrite] [--no-verify]\n"
               "\n"
               "  --kind all    one output file for the whole directory; no\n"
               "                calendar cut. fails on any skipped input.\n"
               "  --max-skew-seconds N\n"
               "                (--kind all) how far a timeline may step back\n"
               "                before it counts as damage. default 60.\n"
               "  --no-verify   (--kind all) skip the ascending/overlap check.\n");
}

}  // namespace

int main(int argc, char** argv) {
  mokuren::mdf::MergerConfig cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        Usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--input")        cfg.input_dir  = next("--input");
    else if (a == "--output")  cfg.output_root = next("--output");
    else if (a == "--venue")   cfg.venue      = next("--venue");
    else if (a == "--symbol")  cfg.symbol     = next("--symbol");
    else if (a == "--kind") {
      const std::string k = next("--kind");
      if (k == "weekly")       cfg.kind = mokuren::mdf::WindowKind::Weekly;
      else if (k == "monthly") cfg.kind = mokuren::mdf::WindowKind::Monthly;
      else if (k == "all")     cfg.kind = mokuren::mdf::WindowKind::All;
      else { std::fprintf(stderr, "bad --kind: %s\n", k.c_str()); return 2; }
    }
    else if (a == "--no-verify") cfg.verify_timeline = false;
    else if (a == "--max-skew-seconds") {
      const long long s = std::atoll(next("--max-skew-seconds"));
      if (s < 0) {
        std::fprintf(stderr, "--max-skew-seconds must be >= 0\n");
        return 2;
      }
      cfg.max_skew_ns = static_cast<int64_t>(s) * 1'000'000'000LL;
    }
    else if (a == "--overwrite") cfg.overwrite_existing = true;
    else if (a == "-h" || a == "--help") { Usage(); return 0; }
    else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); Usage(); return 2; }
  }
  if (cfg.input_dir.empty() || cfg.output_root.empty() ||
      cfg.venue.empty() || cfg.symbol.empty()) {
    Usage();
    return 2;
  }

  mokuren::mdf::Merger merger(std::move(cfg));
  if (!merger.Run()) {
    std::fprintf(stderr, "merge failed: %s\n", merger.last_error().c_str());
    return 1;
  }
  const auto& s = merger.stats();
  std::printf(
      "scanned=%llu skipped=%llu merged=%llu windows=%llu records=%llu "
      "index_repairs=%llu\n",
      static_cast<unsigned long long>(s.files_scanned),
      static_cast<unsigned long long>(s.files_skipped),
      static_cast<unsigned long long>(s.files_merged),
      static_cast<unsigned long long>(s.windows_written),
      static_cast<unsigned long long>(s.records_copied),
      static_cast<unsigned long long>(s.files_index_repair));

  for (const auto& p : s.outputs) std::printf("wrote %s\n", p.c_str());

  // A repaired index is not cosmetic and not fatal. The payload was copied from
  // the chain, so the output is complete; what was wrong is the SOURCE file's
  // own record count and timestamps, which is worth knowing before those files
  // are trusted for anything that reads their trailer instead of their blocks.
  if (s.files_index_repair > 0) {
    std::fprintf(stderr,
                 "warning: %llu input(s) have a trailer index that disagrees "
                 "with their block chain. the merged payload came from the "
                 "chain and is complete; those source files' own record counts "
                 "and timestamps were wrong\n",
                 static_cast<unsigned long long>(s.files_index_repair));
  }

  // Run() returning true only means "no hard error". In the bucketed modes a
  // directory where every input was skipped (all *.part, all unfinalised)
  // reaches here with windows_written == 0 and looks like a clean run. All mode
  // already fails inside Run; checking here keeps the exit code honest for both.
  if (s.windows_written == 0) {
    std::fprintf(stderr,
                 "merge wrote nothing: scanned=%llu skipped=%llu\n",
                 static_cast<unsigned long long>(s.files_scanned),
                 static_cast<unsigned long long>(s.files_skipped));
    return 1;
  }
  return 0;
}
