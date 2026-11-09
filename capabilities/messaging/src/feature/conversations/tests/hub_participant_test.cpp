#include "feature/conversations/ConversationsHub.h"

#include <gtest/gtest.h>
#include <chrono>
#include <functional>
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

// The Chats header's status line (#175): the OS network and the relay's last poll become one state. The 5 s
// "no poll result yet" timer is faked: the test holds the scheduled task and fires it by hand.
class LinkFixture {
public:
  using Health = MeshDeliveryOrchestrator::BriefRelayHealthState;
  using Link = ConversationsHub::LinkState;
  LinkFixture() {
    hub.SetOnLinkStateChanged([this](Link state) { seen.push_back(state); });
    hub.SetLinkGraceScheduler([this](std::chrono::milliseconds delay, std::function<void()> fn) {
      delays.push_back(delay);
      pending = std::move(fn);
    });
  }
  ~LinkFixture() { hub.SetOnLinkStateChanged(nullptr); }
  void Network(bool online, bool tunnel_only = false) {
    NetworkChange change;
    change.generation = ++generation;
    change.current.online = online;
    change.current.tunnel_only = tunnel_only;
    hub.OnLocalNetworkChanged(change);
  }
  void FireTimer() {
    auto fn = std::move(pending);
    pending = nullptr;
    ASSERT_TRUE(fn);
    fn();
  }
  ConversationsHub hub;
  std::vector<Link> seen;
  std::vector<std::chrono::milliseconds> delays;
  std::function<void()> pending;
  uint64_t generation = 0;
};

TEST(HubParticipantTest, LinkIsOnlineWhenTheNetworkIsUpAndTheRelayAnswered) {
  LinkFixture f;
  EXPECT_EQ(f.hub.CurrentLinkState(), LinkFixture::Link::Unknown);
  f.Network(true);
  EXPECT_TRUE(f.seen.empty()) << "network up, no poll result yet: nothing for the first 5 s";
  ASSERT_EQ(f.delays.size(), 1u);
  EXPECT_EQ(f.delays[0], std::chrono::milliseconds(5000));
  f.hub.OnBriefRelayHealthChanged(LinkFixture::Health::Ok);
  EXPECT_EQ(f.seen, (std::vector<LinkFixture::Link>{LinkFixture::Link::Online}));
  f.FireTimer(); // the stale timer does nothing
  EXPECT_EQ(f.seen.size(), 1u);
}

TEST(HubParticipantTest, LinkIsConnectingAtOnceWhenTheRelayPollFails) {
  LinkFixture f;
  f.Network(true);
  f.hub.OnBriefRelayHealthChanged(LinkFixture::Health::Failed);
  EXPECT_EQ(f.seen, (std::vector<LinkFixture::Link>{LinkFixture::Link::Connecting}));
  f.hub.OnBriefRelayHealthChanged(LinkFixture::Health::Ok);
  EXPECT_EQ(f.seen, (std::vector<LinkFixture::Link>{LinkFixture::Link::Connecting, LinkFixture::Link::Online}));
}

TEST(HubParticipantTest, LinkWithNoPollResultBecomesConnectingOnlyWhenTheGraceElapses) {
  LinkFixture f;
  f.Network(true);
  EXPECT_TRUE(f.seen.empty());
  f.FireTimer();
  EXPECT_EQ(f.seen, (std::vector<LinkFixture::Link>{LinkFixture::Link::Connecting}));
}

TEST(HubParticipantTest, LinkIsOfflineWhenTheNetworkIsDownWhateverTheRelaySaid) {
  LinkFixture f;
  f.Network(true);
  f.hub.OnBriefRelayHealthChanged(LinkFixture::Health::Ok);
  f.Network(false);
  EXPECT_EQ(f.seen, (std::vector<LinkFixture::Link>{LinkFixture::Link::Online, LinkFixture::Link::Offline}));
  f.hub.OnBriefRelayHealthChanged(LinkFixture::Health::Failed);
  f.hub.OnBriefRelayHealthChanged(LinkFixture::Health::Ok);
  EXPECT_EQ(f.seen.size(), 2u) << "relay news changes nothing while the network is down";
  // Airplane mode with a VPN left on: only the tunnel is up, which counts as offline.
  f.Network(true);
  EXPECT_EQ(f.seen.back(), LinkFixture::Link::Online);
  f.Network(true, true);
  EXPECT_EQ(f.seen.back(), LinkFixture::Link::Offline);
  EXPECT_EQ(f.hub.NetworkOnline(), 0);
}

TEST(HubParticipantTest, LinkLeavesOfflineForConnectingAtOnceWhenTheNetworkReturns) {
  LinkFixture f;
  f.Network(false);
  EXPECT_EQ(f.seen, (std::vector<LinkFixture::Link>{LinkFixture::Link::Offline}));
  f.Network(true); // health still Unknown: red would be false now, no 5 s wait
  EXPECT_EQ(f.seen, (std::vector<LinkFixture::Link>{LinkFixture::Link::Offline, LinkFixture::Link::Connecting}));
  f.hub.OnBriefRelayHealthChanged(LinkFixture::Health::Ok);
  EXPECT_EQ(f.seen.back(), LinkFixture::Link::Online);
  EXPECT_EQ(f.seen.size(), 3u);
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
