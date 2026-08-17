#include "feature/messaging/Libp2pChatHistoryService.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/messaging/E2eRelayPayloadCodec.h"
#include "base/messaging/SqliteThreadStore.h"
#include "base/p2p/tests/libp2p_ephemeral_listen.h"
#include "feature/messaging/SqlitePskSessionStore.h"
#include "base/p2p/Libp2pHost.h"
#include "base/p2p/PeerSessionManager.h"

#include <filesystem>
#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xb0 + i);
  }
  return dek;
}

class HistoryHarness {
public:
  explicit HistoryHarness(const std::string& suffix)
      : data_dir(std::filesystem::temp_directory_path() / ("pp_browser_libp2p_history_" + suffix)),
        store(data_dir.string()), identity(data_dir.string(), "test"), psk_store(store.ProfileDbPath(), "test") {
    std::filesystem::remove_all(data_dir);
    if (!identity.SetDek(TestDek()) || !psk_store.SetDek(TestDek())) {
      throw std::runtime_error("dek setup failed");
    }
    if (!identity.LoadOrCreate()) {
      throw std::runtime_error("identity load failed");
    }
    {
      LocalIdentity updated = *identity.Get();
      updated.relay_user_id = "relay:responder";
      updated.registered = true;
      if (!identity.Update(updated)) {
        throw std::runtime_error("identity update failed");
      }
    }

    DirectChatTarget target;
    target.peer_identity_kind = "relay_user";
    target.peer_identity_value = "relay:requester";
    target.channel = ThreadChannel::E2e;
    auto created = store.FindOrCreateDirectThread(target, "contact-requester", "Requester");
    if (!created) {
      throw std::runtime_error("thread create failed");
    }
    thread = *created;

    PskSessionRecord psk;
    psk.key = E2eRelayPayloadCodec::ChatTargetFromThread(thread);
    psk.session_epoch = 1;
    const auto master = HexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    if (!master) {
      throw std::runtime_error("psk bytes");
    }
    psk.master_psk_b64 = Base64Encode(*master);
    psk.psk_verified_at = 1;
    if (!psk_store.Save(psk)) {
      throw std::runtime_error("psk save");
    }
  }

  void SeedOutbound(uint64_t seq, const std::string& text) {
    ThreadMessage message;
    message.id = "out-" + std::to_string(seq);
    message.thread_id = thread.id;
    message.sender_contact_id = kLocalSelfContactId;
    message.text = text;
    message.timestamp = static_cast<int64_t>(seq);
    message.delivery = MessageDelivery::Relayed;
    message.relay_visible = true;
    message.transport = MessageTransport::Local;
    message.sender_seq = seq;
    message.session_epoch = 1;
    if (!store.AppendMessage(message)) {
      throw std::runtime_error("append failed");
    }
  }

  std::filesystem::path data_dir;
  SqliteThreadStore store;
  IdentityStore identity;
  SqlitePskSessionStore psk_store;
  Thread thread;
};

class Libp2pChatHistoryServiceTest : public ::testing::Test {
protected:
  void SetUp() override {
    harness_ = std::make_unique<HistoryHarness>("svc");

    PeerSessionConfig config;
    config.dial_timeout = std::chrono::milliseconds(3000);
    config.dial_failure_backoff = std::chrono::milliseconds(100);

    auto responder_started = test::StartEphemeralLoopbackHost(responder_host_, responder_port_);
    ASSERT_TRUE(responder_started) << responder_started.error().message;
    responder_sessions_ = std::make_unique<PeerSessionManager>(responder_host_, config);
    responder_history_ = std::make_unique<Libp2pChatHistoryService>(
        responder_host_, *responder_sessions_, harness_->store, harness_->identity, harness_->psk_store);

    auto client_started = test::StartEphemeralLoopbackHost(client_host_, client_port_);
    ASSERT_TRUE(client_started) << client_started.error().message;
    client_sessions_ = std::make_unique<PeerSessionManager>(client_host_, config);
    client_history_ = std::make_unique<Libp2pChatHistoryService>(client_host_, *client_sessions_, harness_->store,
                                                                   harness_->identity, harness_->psk_store);

    auto responder_id = responder_host_.LocalPeerIdBase58();
    ASSERT_TRUE(responder_id);
    const std::string responder_ma = test::LoopbackP2pMultiaddr(responder_port_, *responder_id);
    ASSERT_TRUE(client_sessions_->RegisterEndpoint("relay:responder", responder_ma));

    responder_history_->Start();
    client_history_->Start();
  }

  void TearDown() override {
    client_history_->Stop();
    responder_history_->Stop();
    client_history_.reset();
    responder_history_.reset();
    client_sessions_.reset();
    responder_sessions_.reset();
    client_host_.Stop();
    responder_host_.Stop();
    harness_.reset();
  }

  int responder_port_ = 0;
  int client_port_ = 0;
  std::unique_ptr<HistoryHarness> harness_;
  Libp2pHost responder_host_;
  Libp2pHost client_host_;
  std::unique_ptr<PeerSessionManager> responder_sessions_;
  std::unique_ptr<PeerSessionManager> client_sessions_;
  std::unique_ptr<Libp2pChatHistoryService> responder_history_;
  std::unique_ptr<Libp2pChatHistoryService> client_history_;
};

TEST_F(Libp2pChatHistoryServiceTest, FetchHistoryRoundTrip) {
  harness_->SeedOutbound(1, "alpha");
  harness_->SeedOutbound(2, "beta");

  ChatHistoryRequest request;
  request.requester_identity_kind = "relay_user";
  request.requester_identity_value = "relay:requester";
  request.peer_identity_kind = "relay_user";
  request.peer_identity_value = "relay:responder";
  request.channel = ThreadChannel::E2e;
  request.session_epoch = 1;
  request.limit = 10;
  request.order = "asc";

  auto response = client_history_->FetchChatHistory(request);
  ASSERT_TRUE(response) << response.error().message;
  ASSERT_EQ(response->messages.size(), 2u);
  EXPECT_EQ(response->messages[0].sender_seq, 1u);
  EXPECT_EQ(response->messages[1].sender_seq, 2u);
}

} // namespace
} // namespace pbr
