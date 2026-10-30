#pragma once

#include <cctype>
#include <cstdint>
#include <string>

namespace mokuren::blofin {

// bloFin clientOrderId: letters, digits and underscores only, at most 32
// characters. Tag is a short ASCII label; the timestamp and a caller-supplied
// sequence make it unique. If the composed id is ever too long the HEAD is
// dropped, not the tail -- the sequence is the unique end, and truncating from
// the right would collapse two orders onto one id (duplicate-order rejection
// instead of a malformed-id rejection, which is harder to diagnose).
//
// hoisted out of the anonymous namespace in blofin_broker.cpp so the id
// builder can be exercised offline: BloFinBroker's only network-free
// constructor (OfflineForTests) does not expose it, and the alternative to a
// unit test is "wait for a live partial close".
inline std::string MakeClientOrderId(const char* tag, int64_t now_ms,
                                     uint64_t seq) {
    std::string t;
    for (const char* p = tag; *p != '\0'; ++p) {
        const char c = *p;
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            t.push_back(c);
    }
    if (t.size() > 8) t.resize(8);
    std::string out = t;
    out.push_back('_');
    out += std::to_string(now_ms);
    out.push_back('_');
    out += std::to_string(seq);
    if (out.size() > 32) out.erase(0, out.size() - 32);
    return out;
}

} // namespace mokuren::blofin
