#include "l2_feed.hpp"

namespace mokuren {

bool CursorL2Feed::Open(const std::string& path, std::string& error,
                        int64_t start_ns, int64_t end_ns) {
  // Re-opening must not carry the previous run's count: the cursor resets
  // itself, and a stale nonzero count would suppress the "no book" warning on
  // the second run of a reused feed.
  frames_delivered_ = 0;
  if (!cursor_.Open(path, mokuren::mdf::Channel::L2, start_ns, end_ns)) {
    error = cursor_.status().error.empty()
                ? std::string("cannot open the book channel of '") + path + "'"
                : cursor_.status().error;
    return false;
  }
  return true;
}

std::string L2StreamProblem(const mokuren::mdf::SessionData& status) {
  // Keyed on the same fields RequireCleanSession used, so the streaming and
  // materialising paths agree about what "the recording dropped something"
  // means. Warnings alone are NOT a problem: an unknown record kind is forward
  // compatibility, and a metadata checksum mismatch costs session labels, not
  // records.
  if (status.error.empty() && status.blocks_skipped == 0 &&
      status.trades_skipped == 0 && status.l2_skipped == 0) {
    return {};
  }

  std::string reason =
      status.error.empty() ? "recording dropped records" : status.error;
  if (status.blocks_skipped > 0) {
    reason += "; " + std::to_string(status.blocks_skipped) +
              " block(s) unusable";
  }
  if (status.trades_skipped > 0) {
    reason += "; " + std::to_string(status.trades_skipped) +
              " trade record(s) unusable";
  }
  if (status.l2_skipped > 0) {
    reason += "; " + std::to_string(status.l2_skipped) +
              " book record(s) unusable, so the run was book-blind";
  }
  if (!status.warnings.empty()) {
    reason += "; first warning: " + status.warnings.front();
  }
  return reason;
}

}  // namespace mokuren
