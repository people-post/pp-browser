#include "lib/people/MeshNodeReputation.h"
#include "lib/people/RelaySetRules.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int kOurWire = 2;
const std::string kSeed = "12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR";

std::vector<mesh::MeshHopCandidate> Hops(const std::vector<std::string>& ids) {
  std::vector<mesh::MeshHopCandidate> hops;
  for (const std::string& id : ids) {
    mesh::MeshHopCandidate hop;
    hop.peer_id = id;
    hops.push_back(hop);
  }
  return hops;
}

std::vector<std::string> Ids(const std::vector<mesh::MeshHopCandidate>& hops) {
  std::vector<std::string> ids;
  for (const auto& hop : hops) {
    ids.push_back(hop.peer_id);
  }
  return ids;
}

mesh::MeshDirectoryNode Row(const std::string& peer, const std::string& account, const std::string& version = {}) {
  mesh::MeshDirectoryNode row;
  row.peer_id = peer;
  row.account_id = account;
  row.node_version = version;
  return row;
}

RelaySetContext Context(const std::vector<mesh::MeshDirectoryNode>& rows, const std::string& floor = {},
                        const MeshNodeReputation* reputation = nullptr, int64_t now = 1000) {
  return MakeRelaySetContext({}, rows, {"/ip4/3.208.41.58/udp/443/adp/1.0.0/p2p/" + kSeed}, floor, reputation, now);
}

// N032 rule 7: one relay per operator before a second of any; an anchor (org seed) is exempt.
TEST(RelaySetRulesTest, ASecondRelayOfAnOperatorIsTriedAfterTheOthers) {
  const auto context = Context(
      {Row("QmA1", "account:a"), Row("QmA2", "account:a"), Row("QmB1", "account:b"), Row(kSeed, "account:org")});
  const auto out = ApplyRelaySetRules(Hops({"QmA1", "QmA2", "QmB1", kSeed, "QmNoRow"}), context);
  EXPECT_EQ(Ids(out), (std::vector<std::string>{"QmA1", "QmB1", kSeed, "QmNoRow", "QmA2"}));
}

// Rules 6 and 8: a demoted node, or any node of a demoted operator, goes last; nothing is dropped.
TEST(RelaySetRulesTest, WhatThisProfileDistrustsGoesLastNeverOut) {
  MeshNodeReputation reputation;
  reputation.NoteContradiction("QmLiar", "account:liar", 1000);
  const auto context =
      Context({Row("QmLiar", "account:liar"), Row("QmSibling", "account:liar"), Row("QmOk", "account:ok")}, {},
              &reputation, 2000);
  const auto out = ApplyRelaySetRules(Hops({"QmLiar", "QmSibling", "QmOk"}), context);
  EXPECT_EQ(Ids(out), (std::vector<std::string>{"QmOk", "QmLiar", "QmSibling"}))
      << "the operator's other node goes with it";
}

// Rule 5's floor: a node said to run older than the floor, or saying nothing, after the rest; no floor, no change.
TEST(RelaySetRulesTest, NodesBelowTheVersionFloorAreTriedLater) {
  const std::vector<mesh::MeshDirectoryNode> rows = {
      Row("QmOld", "account:1", "4.0.0"), Row("QmNew", "account:2", "4.1.0"), Row("QmRc", "account:3", "4.1.0-rc2")};
  const auto hops = Hops({"QmOld", "QmSilent", "QmNew", "QmRc"});
  EXPECT_EQ(Ids(ApplyRelaySetRules(hops, Context(rows))), Ids(hops)) << "no floor";
  EXPECT_EQ(Ids(ApplyRelaySetRules(hops, Context(rows, "4.1.0"))),
            (std::vector<std::string>{"QmNew", "QmRc", "QmOld", "QmSilent"}));
  EXPECT_LT(CompareNodeVersions("4.0.9", "4.1.0"), 0);
  EXPECT_EQ(CompareNodeVersions("4.1.0-rc2", "4.1.0"), 0);
  EXPECT_GT(CompareNodeVersions("10.0.0", "9.9.9"), 0);
}

// Rule 7: an anchor leads, and at most kMaxUnknownBetweenAnchors others come between anchors while anchors remain.
TEST(RelaySetRulesTest, AnAnchorLeadsAndUnknownNodesCannotCrowdThemOut) {
  RelaySetContext context;
  context.anchors = {"S1", "S2"};
  EXPECT_EQ(Ids(AnchorLed(Hops({"U1", "U2", "U3", "U4", "U5", "S1", "S2"}), context)),
            (std::vector<std::string>{"S1", "U1", "U2", "S2", "U3", "U4", "U5"}));
  RelaySetContext none;
  EXPECT_EQ(Ids(AnchorLed(Hops({"U1", "U2"}), none)), (std::vector<std::string>{"U1", "U2"})) << "no anchors";
}

// Rule 6 counts only the direction a liar gains; an upgraded or silent node is never contradicted.
TEST(RelaySetRulesTest, OnlyAClaimInTheLiarsFavourIsContradicted) {
  mesh::MeshDirectoryNode row = Row("QmN", "account:n", "4.1.0");
  row.amp_wire = kOurWire;
  row.features = {"hop_data_lane"};
  ObservedNode seen;
  EXPECT_FALSE(ClaimsContradicted(row, seen, kOurWire)) << "nothing seen";
  seen.answered_other_wire = true;
  EXPECT_TRUE(ClaimsContradicted(row, seen, kOurWire)) << "said ours, answered another";

  ObservedNode told;
  told.told = true;
  told.amp_wire = kOurWire;
  told.node_version = "4.2.0";
  told.features = {"hop_data_lane", "more"};
  EXPECT_FALSE(ClaimsContradicted(row, told, kOurWire)) << "upgraded since it registered";
  told.node_version = "4.0.0";
  EXPECT_TRUE(ClaimsContradicted(row, told, kOurWire)) << "claims newer than it runs";
  told.node_version = "4.1.0";
  told.features = {};
  EXPECT_TRUE(ClaimsContradicted(row, told, kOurWire)) << "claims a feature it lacks";

  mesh::MeshDirectoryNode silent = Row("QmS", "account:s");
  ObservedNode other_wire;
  other_wire.answered_other_wire = true;
  EXPECT_FALSE(ClaimsContradicted(silent, other_wire, kOurWire)) << "a node that said nothing is just old";
}

TEST(RelaySetRulesTest, TheAuditDemotesTheContradictedNodeAndItsOperator) {
  mesh::MeshDirectoryNode liar = Row("QmLiar", "account:liar", "9.0.0");
  mesh::MeshDirectoryNode honest = Row("QmHonest", "account:honest", "4.1.0");
  MeshNodeReputation reputation;
  AuditNodeClaims(
      {liar, honest},
      [](const std::string&) {
        ObservedNode seen;
        seen.told = true;
        seen.amp_wire = kOurWire;
        seen.node_version = "4.1.0";
        return seen;
      },
      reputation, kOurWire, 1000);
  EXPECT_TRUE(reputation.IsDemoted("QmLiar", "", 2000));
  EXPECT_TRUE(reputation.IsDemoted("QmOther", "account:liar", 2000)) << "its operator too";
  EXPECT_FALSE(reputation.IsDemoted("QmHonest", "account:honest", 2000));
}

// Rule 8: a demotion lasts, doubles with strikes up to a cap, is not re-struck while it lasts, and persists.
TEST(MeshNodeReputationTest, DemotionsLastDoubleAndSurviveARestart) {
  const auto* info = ::testing::UnitTest::GetInstance();
  const std::string path =
      (std::filesystem::temp_directory_path() / ("pp_node_reputation_" + std::to_string(info->random_seed()) + ".json"))
          .string();
  std::filesystem::remove(path);
  const int64_t t0 = 1'000'000;
  {
    MeshNodeReputation reputation;
    reputation.SetPersistPath(path);
    reputation.NoteContradiction("QmN", "account:n", t0);
    reputation.NoteContradiction("QmN", "account:n", t0 + 5000);
    EXPECT_EQ(reputation.Strikes("QmN"), 1) << "seen again while demoted: not a new strike";
    EXPECT_TRUE(reputation.IsDemoted("QmN", "", t0 + MeshNodeReputation::kFirstDemotionMs - 1));
    EXPECT_FALSE(reputation.IsDemoted("QmN", "", t0 + MeshNodeReputation::kFirstDemotionMs));
    reputation.NoteContradiction("QmN", "account:n", t0 + MeshNodeReputation::kFirstDemotionMs);
    EXPECT_EQ(reputation.Strikes("QmN"), 2);
    EXPECT_TRUE(reputation.IsDemoted("QmN", "", t0 + 3 * MeshNodeReputation::kFirstDemotionMs - 1)) << "doubled";
  }
  MeshNodeReputation restarted;
  restarted.SetPersistPath(path);
  restarted.Load();
  EXPECT_EQ(restarted.Strikes("QmN"), 2);
  EXPECT_TRUE(restarted.IsDemoted("QmOther", "account:n", t0 + MeshNodeReputation::kFirstDemotionMs + 1));
  std::filesystem::remove(path);

  MeshNodeReputation capped;
  int64_t now = t0;
  for (int i = 0; i < 10; ++i) {
    capped.NoteContradiction("QmC", "", now);
    now += MeshNodeReputation::kMaxDemotionMs;
  }
  EXPECT_FALSE(capped.IsDemoted("QmC", "", now)) << "never longer than the cap";
}

} // namespace
} // namespace pbr
