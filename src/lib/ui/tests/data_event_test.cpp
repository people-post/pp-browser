#include "lib/ui/DataEvent.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

void Fire(const ui::DataEventFunc& handler, const ui::VariantList& args) {
  ui::Event ev;
  handler(ui::DataModelHandle(), ev, args);
}

ui::VariantList Args(std::vector<ui::Variant> values) { return ui::VariantList(values.begin(), values.end()); }

} // namespace

TEST(DataEventTest, NoArgumentsRunsOnEveryEventAndIgnoresExtraArguments) {
  int runs = 0;
  const auto handler = pbr::DataEvent([&runs] { ++runs; });
  Fire(handler, {});
  Fire(handler, Args({ui::Variant(ui::String("extra"))}));
  EXPECT_EQ(runs, 2);
}

TEST(DataEventTest, TypedArgumentsArePassedThrough) {
  std::string got_id;
  int got_index = -1;
  const auto handler = pbr::DataEvent<std::string, int>([&](const std::string& id, int index) {
    got_id = id;
    got_index = index;
  });
  Fire(handler, Args({ui::Variant(ui::String("entry-1")), ui::Variant(3)}));
  EXPECT_EQ(got_id, "entry-1");
  EXPECT_EQ(got_index, 3);
  // An int may come as a numeric string from the template.
  Fire(handler, Args({ui::Variant(ui::String("entry-2")), ui::Variant(ui::String("5"))}));
  EXPECT_EQ(got_id, "entry-2");
  EXPECT_EQ(got_index, 5);
}

TEST(DataEventTest, MissingOrMistypedArgumentsSkipTheHandler) {
  int runs = 0;
  const auto handler =
      pbr::DataEvent<std::string, std::string>([&runs](const std::string&, const std::string&) { ++runs; });
  Fire(handler, {});
  Fire(handler, Args({ui::Variant(ui::String("only one"))}));
  Fire(handler, Args({ui::Variant(7), ui::Variant(ui::String("not a string first"))}));
  EXPECT_EQ(runs, 0);
  Fire(handler, Args({ui::Variant(ui::String("a")), ui::Variant(ui::String("b"))}));
  EXPECT_EQ(runs, 1);
}

TEST(DataEventTest, AtPassesTheEvent) {
  ui::Event* seen = nullptr;
  const auto handler = pbr::DataEventAt([&seen](ui::Event& ev) { seen = &ev; });
  ui::Event ev;
  handler(ui::DataModelHandle(), ev, {});
  EXPECT_EQ(seen, &ev);
}
