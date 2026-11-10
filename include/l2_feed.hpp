#pragma once

#include "market_data/reader.hpp"
#include "trade_event.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace mokuren {

// A source of ascending book frames for one run of the shared backtest loop.
//
// Exists so the loop body is written once and fed from either the legacy
// materialised vector (a CSV / directory dataset) or a SessionCursor streaming
// a .mkn recording. A day of book is ~2 GB of L2Snapshot (688 B per frame) and
// a week does not fit in RAM, so the streaming form is the one a weekly run
// uses; the vector form is the compatibility path for datasets small enough to
// have been loaded whole anyway.
//
// Contract: Front() is strictly non-decreasing in timestamp across
// HasNext()/Advance(). The loop stops catching the book up on the first frame
// that is not older than the current print, which is only sound under that
// ordering -- the cursor guarantees it with a bounded reorder window.
class L2Feed {
 public:
  virtual ~L2Feed() = default;
  virtual bool HasNext() = 0;
  virtual const L2Snapshot& Front() const = 0;
  virtual void Advance() = 0;
};

// Borrows a vector the caller keeps alive for the feed's lifetime. Range
// filtering is the caller's job (DataManager::SliceL2), which is what the
// directory path already did.
class VectorL2Feed final : public L2Feed {
 public:
  explicit VectorL2Feed(const std::vector<L2Snapshot>& source)
      : source_(source) {}
  bool HasNext() override { return index_ < source_.size(); }
  const L2Snapshot& Front() const override { return source_[index_]; }
  void Advance() override { ++index_; }

 private:
  const std::vector<L2Snapshot>& source_;
  size_t index_ = 0;
};

// Streams one recording between two NANOSECOND bounds. Owns the cursor, so a
// worker that constructs its own CursorL2Feed pays one open (header + metadata
// + index) and then block-sized reads -- not a decode of the window.
class CursorL2Feed final : public L2Feed {
 public:
  // Returns false and fills `error` when the file cannot be opened or the
  // cursor refuses the channel. A damaged TAIL is not reported here: the
  // cursor discovers it while streaming, so a caller that commits a result
  // must inspect status() first (see L2StreamProblem).
  //
  // The default bounds are the whole file. They are defaults rather than an
  // overload so a caller that knows its window cannot forget to pass it.
  bool Open(const std::string& path, std::string& error,
            int64_t start_ns = std::numeric_limits<int64_t>::min(),
            int64_t end_ns = std::numeric_limits<int64_t>::max());

  bool HasNext() override { return cursor_.HasNext(); }
  const L2Snapshot& Front() const override { return cursor_.FrontL2(); }
  void Advance() override { cursor_.Advance(); ++frames_delivered_; }

  const mokuren::mdf::SessionData& status() const { return cursor_.status(); }

  // Frames the consumer actually took. Zero after a run means the recording
  // carried no usable book at all, which the skip counters cannot report
  // because nothing was skipped: there was nothing to skip. The vector
  // overload can test all_l2.empty() before the run; a streamed recording can
  // only be asked afterwards, hence the count.
  size_t frames_delivered() const { return frames_delivered_; }

 private:
  mokuren::mdf::SessionCursor cursor_;
  size_t frames_delivered_ = 0;
};

// The counters the materialising loaders used to throw on, as a message.
//
// A cursor hands out data BEFORE it discovers a corrupt tail, where
// DataManager's RequireCleanSession threw before the run started. A caller that
// streams therefore has to ask this before it treats its result as a clean run
// of the dataset. Empty string means clean.
std::string L2StreamProblem(const mokuren::mdf::SessionData& status);

}  // namespace mokuren
