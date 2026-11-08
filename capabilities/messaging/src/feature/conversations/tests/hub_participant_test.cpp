#include "feature/conversations/ConversationsHub.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** Records the hooks it gets, tagged with its name, into a shared log. */
class RecordingParticipant : public HubParticipant {
public:
  RecordingParticipant(std::string name, std::vector<std::string>& log) : name_(std::move(name)), log_(log) {}

  void OnLocalNetworkChanged(const NetworkChange&, const mesh::LocalNetworkChange* local) override {
    log_.push_back(name_ + (local ? (local->attachment_changed ? ":moved" : ":same") : ":baseline"));
  }
  void OnShutdownRequested() override { log_.push_back(name_ + ":abort"); }

private:
  std::string name_;
  std::vector<std::string>& log_;
};

TEST(HubParticipantTest, NetworkChangesReachParticipantsInOrderWithTheMeshsView) {
  std::vector<std::string> log;
  RecordingParticipant calls("calls", log);
  RecordingParticipant broadcast("broadcast", log);
  ConversationsHub hub;
  hub.AddParticipant(calls);
  hub.AddParticipant(broadcast);

  NetworkChange baseline;
  baseline.current.online = true;
  hub.OnLocalNetworkChanged(baseline);
  EXPECT_EQ(log, (std::vector<std::string>{"calls:baseline", "broadcast:baseline"}));

  log.clear();
  NetworkChange moved;
  moved.generation = 1;
  moved.previous.online = true;
  moved.previous.fingerprint = "wifi";
  moved.current.online = true;
  moved.current.fingerprint = "cellular";
  hub.OnLocalNetworkChanged(moved);
  EXPECT_EQ(log, (std::vector<std::string>{"calls:moved", "broadcast:moved"}));
  hub.ClearParticipants();
}

// The Chats header's status line: the OS monitor's online flag reaches the UI callback on the first report (the baseline)
// and on each flip, never twice for the same value; NetworkOnline() holds the latest (-1 until the first report).
TEST(HubParticipantTest, TheDevicesOnlineFlagIsReportedOnFirstReportAndEachFlipOnly) {
  ConversationsHub hub;
  std::vector<std::string> seen;
  hub.SetOnNetworkOnlineChanged([&](bool known, bool online) { seen.push_back(std::string(known ? "k" : "u") + (online ? "1" : "0")); });
  EXPECT_EQ(hub.NetworkOnline(), -1);

  NetworkChange baseline;
  baseline.current.online = false;
  hub.OnLocalNetworkChanged(baseline);
  EXPECT_EQ(seen, (std::vector<std::string>{"k0"}));
  EXPECT_EQ(hub.NetworkOnline(), 0);

  NetworkChange again = baseline;
  again.generation = 1;
  hub.OnLocalNetworkChanged(again);
  EXPECT_EQ(seen.size(), 1u) << "same value, no event";

  NetworkChange back = baseline;
  back.generation = 2;
  back.previous.online = false;
  back.current.online = true;
  hub.OnLocalNetworkChanged(back);
  EXPECT_EQ(seen, (std::vector<std::string>{"k0", "k1"}));
  EXPECT_EQ(hub.NetworkOnline(), 1);
  hub.SetOnNetworkOnlineChanged(nullptr);
}

TEST(HubParticipantTest, TheWayDownRunsInReverseAndClearedParticipantsHearNothing) {
  std::vector<std::string> log;
  RecordingParticipant calls("calls", log);
  RecordingParticipant broadcast("broadcast", log);
  ConversationsHub hub;
  hub.AddParticipant(calls);
  hub.AddParticipant(broadcast);

  hub.AbortParticipantsForShutdown();
  EXPECT_EQ(log, (std::vector<std::string>{"broadcast:abort", "calls:abort"}));

  log.clear();
  hub.ClearParticipants();
  hub.AbortParticipantsForShutdown();
  EXPECT_TRUE(log.empty());
}

} // namespace
} // namespace pbr
