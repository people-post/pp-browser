#include "domain/relay/RelayService.h"

#include <gtest/gtest.h>

#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

using namespace pbr;

// projects/node-relay NR2: pp-node-plus's message relay, empty until NR3 adds the mailbox.

TEST(RelayServiceTest, AStoppedMeshTakesNoService) {
  mesh::MeshHost mesh{mesh::MeshExecutors{}, MetricsRegistry::Global()};
  plus::RelayService relay;
  const auto started = relay.Start(mesh, {});
  ASSERT_FALSE(static_cast<bool>(started));
  EXPECT_NE(started.error().message.find("not started"), std::string::npos);
  EXPECT_FALSE(relay.Started());
}

TEST(RelayServiceTest, StopIsIdempotent) {
  plus::RelayService relay;
  EXPECT_FALSE(relay.Failure().has_value());
  relay.Stop();
  relay.Stop();
  EXPECT_FALSE(relay.Started());
}
