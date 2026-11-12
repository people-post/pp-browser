#include "feature/conversations/MeshNetworkReaction.h"

#include "common/Diag.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pbr {
namespace {

struct Capture : logging::Handler {
  void emit(logging::Level, const std::string& name, const std::string& message) override {
    lines.emplace_back(name, message);
  }
  std::vector<std::pair<std::string, std::string>> lines;
};

// #416: what support sees of a dead socket — the outcome, never the error text (DIAGNOSTICS.md);
// the error goes to the developer log.
TEST(MeshNetworkReactionTest, ADeadSocketIsADiagLineAndItsErrorStaysInTheDeveloperLog) {
  auto diag = std::make_shared<Capture>();
  DiagLog().addHandler(diag);
  auto dev = std::make_shared<Capture>();
  logging::Logger log = logging::getLogger("MeshNetworkReactionTest");
  log.addHandler(dev);

  mesh::UdpSocketCheck check;
  check.reason = mesh::UdpSocketCheckReason::Requested;
  check.reopened = true;
  check.alive = true;
  check.error = "adp udp: sendto dst=[::1]:4001 errno=57";
  ReportUdpSocketCheck(log, check);

  ASSERT_FALSE(diag->lines.empty());
  const std::string& line = diag->lines.back().second;
  EXPECT_NE(line.find("event=mesh.udp_socket reason=foreground was_alive=0 reopened=1 alive=1"), std::string::npos)
      << line;
  EXPECT_EQ(line.find("errno"), std::string::npos);
  ASSERT_FALSE(dev->lines.empty());
  EXPECT_NE(dev->lines.back().second.find("errno=57"), std::string::npos);
}

TEST(MeshNetworkReactionTest, ALiveSocketOnANetworkChangeIsOneQuietDiagLine) {
  auto diag = std::make_shared<Capture>();
  DiagLog().addHandler(diag);
  auto dev = std::make_shared<Capture>();
  logging::Logger log = logging::getLogger("MeshNetworkReactionTest.Live");
  log.addHandler(dev);

  mesh::UdpSocketCheck check;
  check.reason = mesh::UdpSocketCheckReason::NetworkChange;
  check.was_alive = check.alive = true;
  ReportUdpSocketCheck(log, check);

  ASSERT_FALSE(diag->lines.empty());
  EXPECT_NE(diag->lines.back().second.find("reason=network was_alive=1 reopened=0 alive=1"), std::string::npos);
  EXPECT_TRUE(dev->lines.empty());
}

// A receive that failed (iOS's defunct socket) asked for the check: support sees why (#596).
TEST(MeshNetworkReactionTest, ACheckAReceiveErrorAskedForSaysSo) {
  auto diag = std::make_shared<Capture>();
  DiagLog().addHandler(diag);
  logging::Logger log = logging::getLogger("MeshNetworkReactionTest.ReceiveError");
  mesh::UdpSocketCheck check;
  check.reason = mesh::UdpSocketCheckReason::ReceiveError;
  check.reopened = check.alive = true;
  ReportUdpSocketCheck(log, check);
  ASSERT_FALSE(diag->lines.empty());
  EXPECT_NE(diag->lines.back().second.find("reason=receive_error was_alive=0 reopened=1 alive=1"), std::string::npos)
      << diag->lines.back().second;
}

} // namespace
} // namespace pbr
