#pragma once
#include <spdlog/common.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace mokuren {

struct LogEntry {
    uint64_t seq = 0;
    spdlog::level::level_enum level = spdlog::level::info;
    std::string text;
};

class LogRing {
public:
    void Push(spdlog::level::level_enum lvl, std::string text) {
        std::lock_guard<std::mutex> lk(m_);
        LogEntry e{++next_seq_, lvl, std::move(text)};
        buf_.push_back(std::move(e));
        if (buf_.size() > kCapacity) buf_.pop_front();
    }

    // everything strictly newer than `after_seq`. the ui polls once per frame.
    std::vector<LogEntry> SnapshotAfter(uint64_t after_seq) const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<LogEntry> out;
        for (const auto& e : buf_) if (e.seq > after_seq) out.push_back(e);
        return out;
    }

    static constexpr size_t kCapacity = 4000;

private:
    mutable std::mutex    m_;
    std::deque<LogEntry>  buf_;
    uint64_t              next_seq_ = 0;
};

// intentionally leaked. the async worker thread can still call Push during
// spdlog teardown; a static object would be destroyed first and Push would
// touch freed memory.
inline LogRing& GetLogRing() {
    static LogRing* ring = new LogRing();
    return *ring;
}

} // namespace mokuren
