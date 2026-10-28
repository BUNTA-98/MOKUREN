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
               "--venue V --symbol S [--kind weekly|monthly] [--overwrite]\n");
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
      else { std::fprintf(stderr, "bad --kind: %s\n", k.c_str()); return 2; }
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
  std::printf("scanned=%llu skipped=%llu merged=%llu windows=%llu records=%llu\n",
              static_cast<unsigned long long>(s.files_scanned),
              static_cast<unsigned long long>(s.files_skipped),
              static_cast<unsigned long long>(s.files_merged),
              static_cast<unsigned long long>(s.windows_written),
              static_cast<unsigned long long>(s.records_copied));
  return 0;
}
