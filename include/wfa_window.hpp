#pragma once
#include <cstdint>

// one rolling walk-forward window. it lives on its own because both sides of
// the engine need it: DataManager slices a dataset into windows, Mokuren
// replays them. declaring it in mokuren.hpp meant data_manager.hpp had to
// include the whole engine -- factory, aggregators, triggers, ui -- for one
// four-field struct, and that edge is what closed the include cycle with
// replay_manager.hpp.
//
// default-initialised on purpose: a window is built by DataManager and read by
// ReplayWFA, and an unset boundary silently became an epoch-relative slice
// instead of failing.
struct WFAWindow {
  int64_t in_sample_start = 0;
  int64_t in_sample_end = 0;
  int64_t out_of_sample_start = 0;
  int64_t out_of_sample_end = 0;
};
