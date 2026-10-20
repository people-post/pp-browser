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
