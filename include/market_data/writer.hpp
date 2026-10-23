#pragma once

// Writer for the .mkn market-data format (v1).
// Normative specification: docs/market_data_format.md
//
// The reader's constants and this writer's constants are the SAME constants
// (market_data/format.hpp) on purpose. A recorder that keeps its own copy of
// the header layout produces files that are only readable by the version of
// itself that wrote them.

#include "market_data/format.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mokuren::mdf {

// What the session was. Venue-agnostic on purpose: this is embedded in the
// metadata block, and the writer must not depend on the engine's config types
// or a record-only binary drags the whole config layer in with it.
struct SessionInfo {
  std::string venue;
  std::string env;
  std::string category;
  std::string symbol;
  std::string source = "live";  // "live" | "replay"
};

// Streams one .mkn file to disk.
//
// Not thread safe, deliberately: a recording has exactly one producer (the
// streamer's callback thread) and one closer. A mutex here would sit in front
// of every trade print to defend against a race the caller creates by closing
// while the stream is still running -- so the contract is instead "call
// Close() after Stop() returns", which joins the producer.
//
// Records are buffered per kind and flushed as blocks. Blocks of different
// kinds therefore appear in flush order, not arrival order. Order WITHIN a kind
// is preserved exactly, and that is the order the engine depends on (prints in
// the order they hit, deltas in sequence). A reader merges the per-kind block
// streams by timestamp, which is what first_ts_ns/last_ts_ns in each index
// entry are for.
class Writer {
 public:
  struct Options {
    uint32_t max_records_per_block = 4096;
    int64_t  max_block_span_ns = 1'000'000'000;  // 1 second

    // Per-record capacity for the variable-length book records. These size the
    // record STRIDE, so they are fixed for the whole file, not per record.
    uint32_t max_levels_per_side = kMaxBookDepth;
    uint32_t max_changes_per_delta = 128;
  };

  Writer() = default;
  ~Writer();

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  bool Open(const std::string& path, const SessionInfo& info,
            const Options& opts);

  // overload instead of a default argument. Options is a nested type with
  // default member initialisers, and naming it in a default argument of an
  // enclosing class member is ill-formed until that enclosing class is
  // complete ("default member initializer ... required before the end of its
  // enclosing class"). A member function body IS a complete-class context, so
  // the construction is legal here.
  bool Open(const std::string& path, const SessionInfo& info) {
    return Open(path, info, Options{});
  }

  // Flush pending blocks, write metadata and index, patch the file header.
  // Idempotent. Returns false if anything was left unwritten -- a truncated
  // recording must be reported, not silently returned as a short file.
  bool Close();

  bool is_open() const { return file_ != nullptr; }
  uint64_t records_written() const { return records_written_; }
  const std::string& last_error() const { return error_; }

  bool WriteTrade(const TradeRec& rec);

  // `levels` is bids then asks, each side best-first. rec.n_bids and rec.n_asks
  // describe that split and must not exceed Options::max_levels_per_side.
  // Unused slots in the stride are zero-filled, as the format requires.
  bool WriteSnapshot(const BookSnapshotRec& rec, const BookLevel* levels,
                     size_t n_levels);

  bool WriteDelta(const BookDeltaRec& rec, const BookChange* changes);

 private:
  struct PendingBlock {
    RecordKind kind = RecordKind::Trade;
    uint32_t   stride = 0;
    uint32_t   count = 0;
    int64_t    first_ts = 0;
    int64_t    last_ts = 0;
    std::vector<uint8_t> payload;

    bool empty() const { return count == 0; }
    void Reset() { count = 0; first_ts = last_ts = 0; payload.clear(); }
  };

  // Reserves one stride-sized, zero-filled record slot and returns a pointer to
  // it. Nullptr means a flush failed and the record was dropped. The pointer is
  // valid only until the next Reserve on the same block.
  uint8_t* Reserve(PendingBlock& block, RecordKind kind, uint32_t stride,
                   int64_t ts);

  bool MaybeFlush(PendingBlock& block);
  bool Flush(PendingBlock& block);
  bool FlushAll();
  std::string BuildMetadata() const;
  void Fail(const std::string& what);

  std::FILE*  file_ = nullptr;
  Options     opts_{};
  SessionInfo info_{};
  FileHeader  header_{};  // written at Open, patched at Close
  PendingBlock trades_{};
  PendingBlock book_{};
  std::vector<IndexEntry> index_{};
  uint64_t    records_written_ = 0;
  std::string error_;
};

}  // namespace mokuren::mdf
