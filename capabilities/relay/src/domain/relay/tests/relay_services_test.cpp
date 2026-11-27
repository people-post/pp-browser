#include "domain/relay/RelayServices.h"

#include <gtest/gtest.h>

#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

using namespace pbr;

// projects/node-relay NR2: pp-node-relay's relay services, none yet (NR3 adds the mailbox).

TEST(RelayServicesTest, AStoppedMeshTakesNoServices) {
  mesh::MeshHost mesh{mesh::MeshExecutors{}, MetricsRegistry::Global()};
  RelayServices services;
  const auto started = services.Start(mesh, {});
  ASSERT_FALSE(static_cast<bool>(started));
  EXPECT_NE(started.error().message.find("not started"), std::string::npos);
  EXPECT_FALSE(services.Started());
}

TEST(RelayServicesTest, NoneRunYetAndStopIsIdempotent) {
  RelayServices services;
  EXPECT_TRUE(services.Running().empty());
  EXPECT_FALSE(services.Failure().has_value());
  services.Stop();
  services.Stop();
  EXPECT_FALSE(services.Started());
}
