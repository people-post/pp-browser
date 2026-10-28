#include "foundation/diagnostics/DiagnosticsScrub.h"

#include <gtest/gtest.h>

#include <string>

namespace {

std::string Scrub(const std::string& line) {
  pbr::ScrubState state;
  return pbr::ScrubLine(line, state);
}

const std::string kAccount = "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_AbCdE";
const std::string kPeer = "QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG";

}  // namespace

TEST(DiagnosticsScrubTest, PlaceholdersAreNumberedByFirstAppearanceAndStable) {
  pbr::ScrubState state;
  const std::string out = pbr::ScrubLine(
      "a account:" + kAccount + " b " + kPeer + " c account:" + kAccount + " d QmSecondPeerEx4mp1eXYZ123456789abcdefghijkmn e " + kPeer,
      state);
  EXPECT_EQ(out, "a acct_1 b peer_1 c acct_1 d peer_2 e peer_1");
  // The same state keeps numbering across lines.
  EXPECT_EQ(pbr::ScrubLine("again " + kPeer, state), "again peer_1");
}

TEST(DiagnosticsScrubTest, FreshStatesGiveIdenticalOutput) {
  const std::string in = "x " + kPeer + " 10.0.0.1:80 /Users/jane/a.txt relay:r-1.example";
  EXPECT_EQ(Scrub(in), Scrub(in));
  EXPECT_EQ(Scrub(in), "x peer_1 ip_1 path_1 relay_1");
}

TEST(DiagnosticsScrubTest, KnownFormats) {
  EXPECT_EQ(Scrub("call:0123456789abcdef0123456789abcdef ok"), "call_1 ok");
  EXPECT_EQ(Scrub("relay:relay-eu-1.example.net failed"), "relay_1 failed");
  EXPECT_EQ(Scrub("to 192.168.1.20:5000 and 10.0.0.7"), "to ip_1 and ip_2");
  EXPECT_EQ(Scrub("to 192.168.1.20:5000 and 192.168.1.20:6000"), "to ip_1 and ip_1");
  EXPECT_EQ(Scrub("[2001:db8::1]:443 and fe80::1%en0 and 2001:db8:0:0:0:0:0:2"), "ip_1 and ip_2 and ip_3");
  EXPECT_EQ(Scrub("[::1]"), "ip_1");
  EXPECT_EQ(Scrub("relay.example.com:443 localhost:8080"), "ip_1 ip_2");
  EXPECT_EQ(Scrub("/ip4/203.0.113.9/tcp/4001/p2p/" + kPeer), "ip_1/p2p/peer_1");
  EXPECT_EQ(Scrub("/ip6/2001:db8::5/udp/9000"), "ip_1");
  EXPECT_EQ(Scrub("a /Users/jane.doe/Library/x.log b"), "a path_1 b");
  EXPECT_EQ(Scrub("a /home/u/x \"b\""), "a path_1 \"b\"");
  EXPECT_EQ(Scrub("/var/mobile/Containers/Data/Application/6F1B/Library"), "path_1");
  EXPECT_EQ(Scrub("/private/var/folders/x /data/user/0/dev.pp/files /data/data/dev.pp /storage/emulated/0/x"),
            "path_1 path_2 path_3 path_4");
  EXPECT_EQ(Scrub("C:\\\\Users\\\\jane\\\\AppData end"), "path_1 end");
  EXPECT_EQ(Scrub("C:\\Users\\jane\\AppData end"), "path_1 end");
}

TEST(DiagnosticsScrubTest, TimestampsAndSimilarTextAreUntouched) {
  const char* const kKeep[] = {
      "[2026-10-28 08:57:32.329] [INFO] [Config] hello",
      "at 12:30:45",
      "08:57:32.329",
      "elapsed 01:23",
      "mix [01:23:45] done",
      "ratio 3:2",
      "see Foo.cpp:123 and bar.mm:9",
      "version 1.2.3",
      "pbr::Foo and std::string",
      "https://example.com/storage/x",
      "relative home/dir and users/x",
  };
  for (const char* line : kKeep) {
    EXPECT_EQ(Scrub(line), line);
  }
}

TEST(DiagnosticsScrubTest, SelfCheckAcceptsPlainLogText) {
  EXPECT_EQ(pbr::SelfCheck("[2026-10-28 08:57:32.329] [INFO] [ThreadCompactionService] elapsed 01:23 at 0x00007ff812345678 acct_1 peer_2 ip_3\n", {}),
            std::nullopt);
}

TEST(DiagnosticsScrubTest, SelfCheckLongTokenIsIdLikeOnly) {
  for (const char* ok : {"Http2ClientConnectionManager", "CallHopMigrateWorkflow2", "thread_compaction_v2_worker_pool",
                         "AbcdefghijKlmnopqrstuvwX1"}) {
    EXPECT_EQ(pbr::SelfCheck(std::string("x ") + ok + " y", {}), std::nullopt) << ok;
  }
  const std::string base58 = "QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG";  // 46 chars, 7 digits
  EXPECT_EQ(pbr::SelfCheck("x " + base58.substr(0, 44) + " y", {}), std::optional<std::string>("long_token"));
  EXPECT_EQ(pbr::SelfCheck("x " + kAccount + " y", {}), std::optional<std::string>("long_token"));
}

TEST(DiagnosticsScrubTest, SelfCheckDetectors) {
  EXPECT_EQ(pbr::SelfCheck("x 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b y", {}), std::optional<std::string>("hex_run"));
  EXPECT_EQ(pbr::SelfCheck("x " + kPeer + " y", {}), std::optional<std::string>("long_token"));
  EXPECT_EQ(pbr::SelfCheck("x jane@example.com y", {}), std::optional<std::string>("email"));
  EXPECT_EQ(pbr::SelfCheck("x /Users/jane/y", {}), std::optional<std::string>("abs_path"));
  EXPECT_EQ(pbr::SelfCheck("x C:\\\\Users\\\\jane y", {}), std::optional<std::string>("abs_path"));
  EXPECT_EQ(pbr::SelfCheck("x /var/mobile/Containers y", {}), std::optional<std::string>("abs_path"));
}

TEST(DiagnosticsScrubTest, SelfCheckKnownIdentifierPrefix) {
  const std::vector<std::string> known = {"relay-eu-1.example.net"};
  EXPECT_EQ(pbr::SelfCheck("seen relay-eu done", known), std::optional<std::string>("known_identifier"));
  EXPECT_EQ(pbr::SelfCheck("seen relay-e done", known), std::nullopt);  // 7-char prefix is not enough
  EXPECT_EQ(pbr::SelfCheck("seen relay-eu done", {}), std::nullopt);
}
