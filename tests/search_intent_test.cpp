#include "agent/SearchIntent.h"

#include <cassert>

int main() {
  assert(pbr::ShouldProactiveWebSearch("today's stock market summary"));
  assert(pbr::ShouldProactiveWebSearch("What is the latest news about AI?"));
  assert(pbr::ShouldProactiveWebSearch("latest news headlines"));
  assert(pbr::ShouldProactiveWebSearch("news about climate change"));
  assert(pbr::ShouldProactiveWebSearch("Show me some today's headlines"));
  assert(!pbr::ShouldProactiveWebSearch("explain how binary search works"));
  assert(!pbr::ShouldProactiveWebSearch("help"));

  assert(pbr::WantsNewsHeadlines("Show me some today's headlines"));
  assert(pbr::WantsNewsHeadlines("What is the latest news about AI?"));
  assert(!pbr::WantsNewsHeadlines("current bitcoin price"));

  assert(pbr::BuildWebSearchQuery("Show me some today's headlines") == "breaking news");
  assert(pbr::BuildWebSearchQuery("What is the latest news about AI?") == "ai news");
  assert(pbr::BuildWebSearchQuery("current bitcoin price") == "current bitcoin price");

  assert(pbr::ShouldProactivePeopleDiscovery("Find someone on the network"));
  assert(pbr::ShouldProactivePeopleDiscovery("search people named alice"));
  assert(pbr::ShouldProactiveContactsList("Show my contacts"));
  assert(!pbr::ShouldProactivePeopleDiscovery("explain binary search"));

  assert(pbr::BuildPeopleSearchQuery("Find someone on the network").empty());
  assert(pbr::BuildPeopleSearchQuery("search people alice") == "alice");

  return 0;
}
