#include "feature/messaging/CallTopologyController.h"
#include "feature/messaging/CallTopologyRelayDeps.h"

#include "base/media/CallMediaEngine.h"
#include "base/messaging/CallControlCodec.h"
#include "base/messaging/CallSessionStore.h"
#include "base/messaging/SoftMigrateLogic.h"
#include "base/messaging/SqliteThreadStore.h"
#include "base/people/ContactsStore.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pbr {
namespace {

class FakeTopologyHost final : public CallTopologyHost {
public:
  Roe<std::string> TopologyLocalIdentity() const override {
    if (local_identity.empty()) {
      return Error("no local identity");
    }
    return local_identity;
  }

  Roe<void> TopologyLeaveCall(const std::string& call_id) override {
    leave_calls.push_back(call_id);
    return {};
  }

  Roe<void> TopologyFanOutToJoined(const std::string& call_id, CallControlType type,
                                   const std::string& detail_json, const std::string& /*display*/,
                                   const std::string& /*skip_identity*/) override {
    FanOut f;
    f.call_id = call_id;
    f.type = type;
    f.detail_json = detail_json;
    fanouts.push_back(std::move(f));
    return {};
  }

  Roe<void> TopologySendDirect(const std::string& /*peer_identity*/, CallControlType /*type*/,
                               const std::string& /*detail_json*/,
                               const std::string& /*display*/) override {
    return {};
  }

  void TopologyNotifyRingChanged() override { ++ring_notifies; }
  void TopologySetLastMediaError(std::string message) override { last_media_error = std::move(message); }
  void TopologyNoteMediaAttempted(const std::string& call_id) override {
    media_attempted.push_back(call_id);
  }
  void TopologyBindMediaCallId(const std::string& /*call_id*/) override {}
  void TopologyClearMediaPeerIdentity() override {}

  struct FanOut {
    std::string call_id;
    CallControlType type = CallControlType::CallEnded;
    std::string detail_json;
  };

  std::string local_identity = "relay:B";
  std::vector<std::string> leave_calls;
  std::vector<FanOut> fanouts;
  std::vector<std::string> media_attempted;
  std::string last_media_error;
  int ring_notifies = 0;
};

class FakeDialRegistry final : public IDialRegistry {
public:
  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    endpoints[peer_key] = multiaddr;
    return {};
  }

  bool IsDialable(const std::string& peer_key) const override {
    return endpoints.find(peer_key) != endpoints.end() || force_dialable.count(peer_key) > 0;
  }

  std::optional<std::string> PreferredMultiaddr(const std::string& peer_key) const override {
    const auto it = endpoints.find(peer_key);
    if (it != endpoints.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  void ClearDialBackoff(const std::string& /*peer_key*/) override {}
  void ClearCallMediaCircuitHop(const std::string& /*peer_key*/) override {}

  std::unordered_map<std::string, std::string> endpoints;
  std::unordered_map<std::string, bool> force_dialable;
};

class FakeMediaRelayClient final : public IMediaRelayClient {
public:
  Roe<std::string> LocalPeerIdBase58() const override {
    return local_peer_id;
  }

  Roe<MediaRelayQuote> RequestQuote(const std::string& hop_peer_key,
                                    const MediaRelayQuoteRequest& request,
                                    int /*timeout_ms*/) override {
    ++quote_calls;
    last_quote_hop = hop_peer_key;
    last_quote_call_id = request.call_id;
    if (!quote_ok) {
      return Error(quote_error);
    }
    MediaRelayQuote q;
    q.ok = true;
    q.quote_id = "quote-" + std::to_string(quote_calls);
    q.a_up_bps = 64000;
    return q;
  }

  Roe<MediaRelayAttachResult> AcceptAndAttach(const std::string& /*hop_peer_key*/,
                                              const std::string& /*quote_id*/,
                                              const std::string& /*call_id*/,
                                              const std::string& /*auth_stub*/,
                                              std::function<void(MediaDataFrame)> /*on_frame*/,
                                              int /*timeout_ms*/) override {
    ++attach_calls;
    if (!attach_ok) {
      return Error(attach_error);
    }
    attached_ = true;
    MediaRelayAttachResult r;
    r.ok = true;
    r.session_token = "tok";
    return r;
  }

  Roe<void> Subscribe(uint32_t /*stream_id*/, uint16_t /*channel_id*/) override { return {}; }
  Roe<void> SendFrame(const MediaDataFrame& /*frame*/) override { return {}; }
  void Detach() override { attached_ = false; }
  bool IsAttached() const override { return attached_; }

  std::string local_peer_id = "12D3KooWLocal";
  bool quote_ok = true;
  bool attach_ok = true;
  std::string quote_error = "media-relay stream open failed: protocol not supported";
  std::string attach_error = "attach failed";
  int quote_calls = 0;
  int attach_calls = 0;
  std::string last_quote_hop;
  std::string last_quote_call_id;

private:
  bool attached_ = false;
};

class CallTopologyControllerTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_topo_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->ListThreads());
    sessions_ = std::make_unique<CallSessionStore>(store_->ProfileDbPath());
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    media_ = std::make_unique<CallMediaEngine>();
    host_ = std::make_unique<FakeTopologyHost>();
    dial_ = std::make_unique<FakeDialRegistry>();
    relay_ = std::make_unique<FakeMediaRelayClient>();
    topo_ = std::make_unique<CallTopologyController>(*host_, *sessions_, *contacts_, *media_);

    CallTopologyController::MediaRelayDeps deps;
    deps.relay = relay_.get();
    deps.dial = dial_.get();
    deps.bootstrap_peers = {
        "/ip4/1.2.3.4/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"};
    deps.prefer_contacts = false;
    topo_->SetMediaRelayDeps(std::move(deps));
    dial_->force_dialable["12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"] = true;
  }

  void TearDown() override {
    topo_.reset();
    media_.reset();
    contacts_.reset();
    sessions_.reset();
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  void SeedJoinedCall(const std::string& call_id, const std::vector<std::string>& identities,
                      int64_t base_joined_at) {
    CallSession session;
    session.call_id = call_id;
    session.origin_thread_id = "thread-1";
    session.media_mode = CallMediaMode::Voice;
    session.state = CallSessionState::Active;
    session.created_at = base_joined_at;
    session.media_epoch = 1;
    session.media_key_id = "mk:1";
    ASSERT_TRUE(sessions_->UpsertSession(session));
    for (size_t i = 0; i < identities.size(); ++i) {
      CallParticipant p;
      p.call_id = call_id;
      p.identity = identities[i];
      p.state = CallParticipantState::Joined;
      p.joined_at = base_joined_at + static_cast<int64_t>(i);
      ASSERT_TRUE(sessions_->UpsertParticipant(p));
    }
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<CallSessionStore> sessions_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<CallMediaEngine> media_;
  std::unique_ptr<FakeTopologyHost> host_;
  std::unique_ptr<FakeDialRegistry> dial_;
  std::unique_ptr<FakeMediaRelayClient> relay_;
  std::unique_ptr<CallTopologyController> topo_;
};

TEST_F(CallTopologyControllerTest, LocalJoinedWithoutHintDoesNotQuote) {
  const std::string call_id = "call:wait";
  SeedJoinedCall(call_id, {"relay:A", "relay:B", "relay:C"}, 1000);
  host_->local_identity = "relay:C";

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::LocalJoinedWithoutHint);
  ASSERT_TRUE(ok);
  EXPECT_EQ(relay_->quote_calls, 0);
  EXPECT_TRUE(host_->fanouts.empty());
}

TEST_F(CallTopologyControllerTest, MidCallNonInitiatorDoesNotQuote) {
  const std::string call_id = "call:mid-b";
  SeedJoinedCall(call_id, {"relay:A", "relay:B", "relay:C"}, 1000);
  host_->local_identity = "relay:B";

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::RemoteAcceptObserved);
  ASSERT_TRUE(ok);
  EXPECT_EQ(relay_->quote_calls, 0);
  EXPECT_TRUE(host_->fanouts.empty());
}

TEST_F(CallTopologyControllerTest, MidCallInitiatorPicksAndQuotes) {
  const std::string call_id = "call:mid";
  // A earliest (sticky initiator / payer); picks on roster or accept.
  SeedJoinedCall(call_id, {"relay:A", "relay:B", "relay:C"}, 1000);
  host_->local_identity = "relay:A";

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::JoinedCountObserved);
  EXPECT_GE(relay_->quote_calls, 1);
  EXPECT_EQ(relay_->last_quote_call_id, call_id);
  if (ok) {
    ASSERT_FALSE(host_->fanouts.empty());
    EXPECT_EQ(host_->fanouts.back().type, CallControlType::CallSfuAttach);
    auto decoded = CallControlCodec::DecodeSfuAttach(host_->fanouts.back().detail_json);
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded->quote_id.empty()) << "fan-out must not reuse consumed quote_id";
    EXPECT_FALSE(decoded->hop_peer_id.empty());
  }
}

TEST_F(CallTopologyControllerTest, PickFailsSurfacesStreamOpenError) {
  const std::string call_id = "call:fail";
  SeedJoinedCall(call_id, {"relay:A", "relay:B", "relay:C"}, 1000);
  host_->local_identity = "relay:A";
  relay_->quote_ok = false;
  relay_->quote_error = "media-relay stream open failed: protocol not supported";

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::RemoteAcceptObserved);
  ASSERT_FALSE(ok);
  EXPECT_NE(ok.error().message.find("stream open failed"), std::string::npos);
  EXPECT_TRUE(host_->fanouts.empty());
}

TEST_F(CallTopologyControllerTest, AttachRejectsSelfHop) {
  CallSfuAttachDetail attach;
  attach.call_id = "call:self";
  attach.hop_peer_id = relay_->local_peer_id;
  attach.hop_multiaddr = "/ip4/127.0.0.1/tcp/1/p2p/" + relay_->local_peer_id;
  auto ok = topo_->AttachLocalToSfu(attach.call_id, attach);
  ASSERT_FALSE(ok);
  EXPECT_NE(ok.error().message.find("self"), std::string::npos);
  EXPECT_EQ(relay_->quote_calls, 0);
}

TEST_F(CallTopologyControllerTest, IceRecoverNonCoordinatorDoesNotQuote) {
  const std::string call_id = "call:ice";
  SeedJoinedCall(call_id, {"relay:A", "relay:B", "relay:C"}, 1000);
  host_->local_identity = "relay:B"; // coordinator is relay:A

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::IceRecover);
  ASSERT_TRUE(ok);
  EXPECT_EQ(relay_->quote_calls, 0);
}

} // namespace
} // namespace pbr
