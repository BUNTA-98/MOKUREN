// Offline coverage for the bloFin clientOrderId builder. The id is the only
// thing standing between a retried order and a duplicate fill, and the venue
// rejects anything that is not [A-Za-z0-9_]{1,32}. Exercised here rather than
// through BloFinBroker because its only network-free constructor
// (OfflineForTests) does not expose the builder.
#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <string>

#include "blofin_client_order_id.hpp"

using mokuren::blofin::MakeClientOrderId;

namespace {

bool IsVenueLegal(const std::string& id) {
  if (id.empty() || id.size() > 32) return false;
  for (const char c : id) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
      return false;
  }
  return true;
}

} // namespace

TEST_CASE("blofin clientOrderId: shape and legality", "[blofin]") {
  SECTION("a plain tag composes tag_ts_seq") {
    const std::string id = MakeClientOrderId("mok", 1700000000000LL, 7);
    CHECK(id == "mok_1700000000000_7");
    CHECK(IsVenueLegal(id));
  }

  SECTION("every call site's tag is legal") {
    for (const char* tag : {"mok", "emg", "close"}) {
      const std::string id = MakeClientOrderId(tag, 1700000000000LL, 1);
      INFO("tag: " << tag);
      CHECK(IsVenueLegal(id));
    }
  }

  SECTION("illegal characters are stripped from the tag") {
    const std::string id = MakeClientOrderId("m-o.k", 1700000000000LL, 1);
    CHECK(id == "mok_1700000000000_1");
    CHECK(IsVenueLegal(id));
  }

  SECTION("an empty tag still yields a legal id") {
    const std::string id = MakeClientOrderId("", 1700000000000LL, 1);
    CHECK(id == "_1700000000000_1");
    CHECK(IsVenueLegal(id));
  }
}

TEST_CASE("blofin clientOrderId: length and uniqueness", "[blofin]") {
  SECTION("a long tag is truncated to 8 characters") {
    const std::string id =
        MakeClientOrderId("abcdefghijklmnop", 1700000000000LL, 1);
    CHECK(id == "abcdefgh_1700000000000_1");
    CHECK(IsVenueLegal(id));
  }

  SECTION("an over-long id drops the head, never the tail") {
    // a 19-digit timestamp plus a large sequence pushes past 32; the sequence
    // is the unique end, so it must survive the truncation.
    const std::string id =
        MakeClientOrderId("abcdefgh", 9223372036854775807LL, 123456789);
    CHECK(id.size() == 32);
    CHECK(IsVenueLegal(id));
    CHECK(id.substr(id.size() - 9) == "123456789");
  }

  SECTION("the sequence distinguishes otherwise identical ids") {
    const std::string a = MakeClientOrderId("mok", 1700000000000LL, 1);
    const std::string b = MakeClientOrderId("mok", 1700000000000LL, 2);
    CHECK(a != b);
  }

  SECTION("the timestamp distinguishes otherwise identical ids") {
    const std::string a = MakeClientOrderId("mok", 1700000000000LL, 1);
    const std::string b = MakeClientOrderId("mok", 1700000000001LL, 1);
    CHECK(a != b);
  }
}
