#include "app/node/NodeCompatFloor.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

// A2 (SERVICE_ENDPOINTS § Client compatibility, pp-node): below the relay's min_node_version the node stops asking
// the relay (no registration, no heartbeat) and keeps its mesh; read at start and once a day; fail open.

struct Relay {
  int fetches = 0;
  bool fail = false;
  std::string floor;
  int64_t now_ms = 1'000;

  NodeCompatFloor Floor(const std::string& node_version) {
    return NodeCompatFloor(
        [this]() -> Roe<ClientCompatDocument> {
          ++fetches;
          if (fail) {
            return Error("relay unreachable");
          }
          ClientCompatDocument doc;
          doc.schema_version = 1;
          doc.min_node_version = floor;
          return doc;
        },
        node_version, [this]() { return now_ms; });
  }
};

TEST(NodeCompatFloorTest, BelowTheFloorTheRelayIsNotAskedAndAboveItIs) {
  Relay relay;
  relay.floor = "10.0.0";
  NodeCompatFloor below = relay.Floor("1.0.0-rc16");
  EXPECT_FALSE(below.RelayCallsAllowed());
  EXPECT_EQ(below.Floor(), "10.0.0");
  NodeCompatFloor at = relay.Floor("10.0.0-rc1");
  EXPECT_TRUE(at.RelayCallsAllowed()) << "a prerelease meets its release's floor, as the relay compares";
  relay.floor = "";
  NodeCompatFloor none = relay.Floor("1.0.0");
  EXPECT_TRUE(none.RelayCallsAllowed()) << "no floor";
}

TEST(NodeCompatFloorTest, ReadOnceADayAndAFailedReadKeepsTheLastVerdict) {
  Relay relay;
  relay.fail = true;
  NodeCompatFloor floor = relay.Floor("1.0.0");
  EXPECT_TRUE(floor.RelayCallsAllowed()) << "none read yet: fail open";
  relay.fail = false;
  relay.floor = "10.0.0";
  EXPECT_TRUE(floor.RelayCallsAllowed()) << "not read again before a day";
  EXPECT_EQ(relay.fetches, 1);
  relay.now_ms += NodeCompatFloor::kRecheckMs;
  EXPECT_FALSE(floor.RelayCallsAllowed());
  EXPECT_EQ(relay.fetches, 2);
  relay.fail = true;
  relay.now_ms += NodeCompatFloor::kRecheckMs;
  EXPECT_FALSE(floor.RelayCallsAllowed()) << "an unreachable relay keeps the floor read last";
  relay.fail = false;
  relay.floor = "1.0.0";
  relay.now_ms += NodeCompatFloor::kRecheckMs;
  EXPECT_TRUE(floor.RelayCallsAllowed()) << "the floor lowered: asked again";
}

TEST(NodeCompatFloorTest, TheErrorSaysWhatToDo) {
  const std::string text = NodeCompatFloor::BelowMessage("1.0.0", "10.0.0");
  EXPECT_NE(text.find("below the relay's min_node_version 10.0.0"), std::string::npos);
  EXPECT_NE(text.find("Upgrade it"), std::string::npos);
  EXPECT_NE(text.find("the mesh keeps running"), std::string::npos);
}

} // namespace
} // namespace pbr
