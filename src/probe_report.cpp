#include "probe_report.hpp"

#include <sstream>

namespace mokuren::integrity {
namespace {

std::string Pad(const std::string& s, size_t width) {
  return s.size() >= width ? s : s + std::string(width - s.size(), ' ');
}

std::string Row(const std::string& venue, const ChannelReport& ch) {
  std::ostringstream os;
  os << Pad(venue, 12) << "  " << Pad(ch.channel, 6) << "  "
     << (ch.passed() ? "ok  " : "FAIL") << "  (" << ch.delivered << " msgs, "
     << ch.rejected() << " rejected";
  if (ch.fatal > 0) {
    os << ", " << CodeName(ch.first_fatal);
  } else if (ch.recoverable > 0) {
    os << ", first: " << CodeName(ch.first_recoverable);
  }
  if (ch.required && ch.delivered < ch.min_required) {
    // a silent channel and a channel that produced malformed frames both read
    // as FAIL, so say which one this is: the number it had to reach.
    os << ", needed " << ch.min_required;
  }
  os << ")\n";
  return os.str();
}

}  // namespace

std::string ProbeReport::ToString() const {
  std::string out;
  out += Row(venue, trades);
  out += Row(venue, l2);

  std::ostringstream os;
  os << Pad(venue, 12) << "  " << Pad("gate", 6) << "  "
     << (passed() ? "ok  " : "FAIL") << "  (" << elapsed_ms << " ms";
  if (reconnects > 0) os << ", " << reconnects << " reconnect(s)";
  os << ")\n";
  if (!reason.empty()) os << "  reason: " << reason << "\n";
  out += os.str();
  return out;
}

}  // namespace mokuren::integrity
