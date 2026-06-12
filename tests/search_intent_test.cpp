#include "agent/SearchIntent.h"

#include <cassert>

int main() {
  assert(pbr::ShouldProactiveWebSearch("today's stock market summary"));
  assert(pbr::ShouldProactiveWebSearch("What is the latest news about AI?"));
  assert(pbr::ShouldProactiveWebSearch("current bitcoin price"));
  assert(!pbr::ShouldProactiveWebSearch("explain how binary search works"));
  assert(!pbr::ShouldProactiveWebSearch("help"));
  return 0;
}
