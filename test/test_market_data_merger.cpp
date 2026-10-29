#include "market_data/merger.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("merger rejects an out-of-range window key", "[merger]") {
  using mokuren::mdf::WindowKind;
  // Monthly: -1 is what WindowKeyFromNs returns when the day string could not
  // be formatted; 202413 is a month that does not exist. Both must be rejected
  // rather than name an output file after data it does not contain.
  CHECK(mokuren::mdf::WindowLabelFromKey(-1, WindowKind::Monthly).empty());
  CHECK(mokuren::mdf::WindowLabelFromKey(202413, WindowKind::Monthly).empty());
  CHECK(mokuren::mdf::WindowLabelFromKey(202401, WindowKind::Monthly) ==
        "2024-01");
  // Weekly: a negative key would otherwise format as a plausible pre-epoch
  // date; the guard matches the monthly branch's lower bound.
  CHECK(mokuren::mdf::WindowLabelFromKey(-1, WindowKind::Weekly).empty());
}
