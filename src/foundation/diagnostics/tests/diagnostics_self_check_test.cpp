#include "foundation/diagnostics/DiagnosticsSelfCheck.h"

#include "common/Diag.h"

#include <gtest/gtest.h>

#include <string>

namespace {

const std::string kAccount = "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_AbCdE";
const std::string kPeer = "QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG";

}  // namespace

// The check is the last gate behind shareable lines: what they write must pass it, or every send is refused.
TEST(DiagnosticsSelfCheckTest, ShareableLinesPassIt) {
  const std::string line = pbr::DiagLine("relay.http_failed")
                               .Ref("peer", "peer:" + kPeer)
                               .Ref("account", "account:" + kAccount)
                               .Call("call", "call:0123456789abcdef0123456789abcdef")
                               .Fail("error", pbr::Error::Make(4, 2, "x"))
                               .Add("ms", 1234)
                               .str();
  EXPECT_EQ(pbr::SelfCheck("[2026-10-28 08:57:32.329] [INFO] [Diag] " + line + "\n", {"account:" + kAccount, kPeer}),
            std::nullopt)
      << line;
}

TEST(DiagnosticsSelfCheckTest, SelfCheckAcceptsPlainLogText) {
  EXPECT_EQ(pbr::SelfCheck("[2026-10-28 08:57:32.329] [INFO] [Diag] elapsed 01:23 at 0x00007ff812345678 peer=3fa9c2d1\n", {}),
            std::nullopt);
}

TEST(DiagnosticsSelfCheckTest, SelfCheckLongTokenIsIdLikeOnly) {
  for (const char* ok : {"Http2ClientConnectionManager", "CallHopMigrateWorkflow2", "thread_compaction_v2_worker_pool",
                         "AbcdefghijKlmnopqrstuvwX1"}) {
    EXPECT_EQ(pbr::SelfCheck(std::string("x ") + ok + " y", {}), std::nullopt) << ok;
  }
  const std::string base58 = "QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG";  // 46 chars, 7 digits
  EXPECT_EQ(pbr::SelfCheck("x " + base58.substr(0, 44) + " y", {}), std::optional<std::string>("long_token"));
  EXPECT_EQ(pbr::SelfCheck("x " + kAccount + " y", {}), std::optional<std::string>("long_token"));
}

TEST(DiagnosticsSelfCheckTest, SelfCheckDetectors) {
  EXPECT_EQ(pbr::SelfCheck("x 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b y", {}), std::optional<std::string>("hex_run"));
  EXPECT_EQ(pbr::SelfCheck("x " + kPeer + " y", {}), std::optional<std::string>("long_token"));
  EXPECT_EQ(pbr::SelfCheck("x jane@example.com y", {}), std::optional<std::string>("email"));
  EXPECT_EQ(pbr::SelfCheck("x /Users/jane/y", {}), std::optional<std::string>("abs_path"));
  EXPECT_EQ(pbr::SelfCheck("x C:\\\\Users\\\\jane y", {}), std::optional<std::string>("abs_path"));
  EXPECT_EQ(pbr::SelfCheck("x /var/mobile/Containers y", {}), std::optional<std::string>("abs_path"));
}

TEST(DiagnosticsSelfCheckTest, KnownIdentifierSchemeIsStrippedBeforeMatching) {
  const std::string bare = "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_AbCdE";
  const std::vector<std::string> known = {"account:" + bare, "peer:ab"};
  EXPECT_EQ(pbr::SelfCheck("[t] [INFO] [Application] saw " + bare + " here", known),
            std::optional<std::string>("known_identifier"));
  EXPECT_EQ(pbr::SelfCheck("[t] [INFO] [Application] state account:none", known), std::nullopt);
  EXPECT_EQ(pbr::SelfCheck("[t] [INFO] [Application] ab", known), std::nullopt);  // too short to be a needle
}

TEST(DiagnosticsSelfCheckTest, SelfCheckKnownIdentifierPrefix) {
  const std::vector<std::string> known = {"relay-eu-1.example.net"};
  EXPECT_EQ(pbr::SelfCheck("seen relay-eu done", known), std::optional<std::string>("known_identifier"));
  EXPECT_EQ(pbr::SelfCheck("seen relay-e done", known), std::nullopt);  // 7-char prefix is not enough
  EXPECT_EQ(pbr::SelfCheck("seen relay-eu done", {}), std::nullopt);
}
