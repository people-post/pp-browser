#include "domain/delivery/AmpAccountCardTransport.h"

#include "mesh/testing/mesh_test_harness.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

namespace pbr {
namespace {

TEST(AmpAccountCardTransportTest, OneRequestOneAnswerNamingTheAskingDevice) {
  ASSERT_GE(sodium_init(), 0);
  auto created = mesh::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created));
  auto harness = std::move(*created);
  ASSERT_TRUE(static_cast<bool>(harness->chat_a().RegisterEndpoint("b", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->chat_b().RegisterEndpoint("a", harness->ma_a)));
  AmpAccountCardTransport a(harness->chat_a(), [&] { harness->PumpBoth(); }, {}, harness->MakePostIoA(),
                            harness->MakePostAfterA());
  AmpAccountCardTransport b(harness->chat_b(), [&] { harness->PumpBoth(); }, {}, harness->MakePostIoB(),
                            harness->MakePostAfterB());
  a.Start();
  b.Start();

  std::string asked_by;
  b.SetAnswer([&](const AccountCardRequest& request, const std::string& remote_peer_id) {
    asked_by = remote_peer_id;
    return AccountCardReply{.key_card = KeyCard{.account_id = request.account_id, .signing_public_key_b64 = "s",
                                                .kem_public_key_b64 = "k", .kem_binding_signature_b64 = "b"}};
  });

  std::mutex mu;
  std::condition_variable cv;
  std::optional<Roe<AccountCardReply>> got;
  a.FetchAsync("b", AccountCardRequest{.account_id = "account:bob"}, [&](Roe<AccountCardReply> reply) {
    std::lock_guard lock(mu);
    got = std::move(reply);
    cv.notify_one();
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (std::chrono::steady_clock::now() < deadline) {
    harness->PumpBoth();
    std::unique_lock lock(mu);
    if (cv.wait_for(lock, std::chrono::milliseconds(10), [&] { return got.has_value(); })) {
      break;
    }
  }
  ASSERT_TRUE(got.has_value());
  ASSERT_TRUE(*got) << got->error().message;
  ASSERT_TRUE((*got)->key_card.has_value());
  EXPECT_EQ((*got)->key_card->account_id, "account:bob");
  EXPECT_EQ(asked_by, harness->peer_id_a) << "the server sees the link's authenticated PeerId";
  a.Stop();
  b.Stop();
}

} // namespace
} // namespace pbr
