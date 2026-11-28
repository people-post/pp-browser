#include "domain/relay/RelayService.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

using namespace pbr;

namespace {

std::filesystem::path FreshDir() {
  return std::filesystem::temp_directory_path() /
         ("pp-relay-service-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

} // namespace

// projects/node-relay NR3: off until its settings enable it (Hongwei, 2026-11-27).
TEST(RelayServiceTest, OffItStartsNothingAndMakesNoFolder) {
  mesh::MeshHost mesh{mesh::MeshExecutors{}, MetricsRegistry::Global()};
  relay::RelayService relay;
  relay::RelayService::Options options;
  options.work_dir = (FreshDir() / "relay").string();
  ASSERT_TRUE(static_cast<bool>(relay.Start(mesh, options)));
  EXPECT_FALSE(relay.Started());
  EXPECT_EQ(relay.Store(), nullptr);
  EXPECT_FALSE(std::filesystem::exists(options.work_dir)) << "no folder, so no dormant data elsewhere";
}

TEST(RelayServiceTest, OnItNeedsAStartedMesh) {
  mesh::MeshHost mesh{mesh::MeshExecutors{}, MetricsRegistry::Global()};
  relay::RelayService relay;
  relay::RelayService::Options options;
  options.work_dir = (FreshDir() / "relay").string();
  options.settings.enabled = true;
  const auto started = relay.Start(mesh, options);
  ASSERT_FALSE(static_cast<bool>(started));
  EXPECT_NE(started.error().message.find("not started"), std::string::npos);
  EXPECT_FALSE(relay.Started());
}

TEST(RelayServiceTest, StopIsIdempotent) {
  relay::RelayService relay;
  EXPECT_FALSE(relay.Failure().has_value());
  relay.Stop();
  relay.Stop();
  EXPECT_FALSE(relay.Started());
}
