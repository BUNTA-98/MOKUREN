#pragma once
#include <cstdint>

struct alignas(32) TradeEvent{
  int64_t timestamp;
  double price;
  double quantity;
  bool is_buyer_maker;
};
