#include "lib/calls/CallMediaKeyExchange.h"

#include "lib/calls/CallControlCodec.h"
#include "lib/calls/CallMediaKeyStore.h"
#include "lib/calls/CallSessionStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include "common/PbrCompat.h"

namespace {
using namespace pbr;

ByteVector Bytes(uint8_t seed, size_t size = 32) {
  ByteVector bytes(size);
  for (size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<uint8_t>(seed + i);
  }
  return bytes;
}

class CallMediaKeyExchangeTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_call_media_key_exchange_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    const std::string db = (dir_ / "profile.db").string();
    keys_ = std::make_unique<CallMediaKeyStore>(db);
    ASSERT_TRUE(keys_->SetDek(Bytes(0xa0, kDataEncryptionKeySize)));
    sessions_ = std::make_unique<CallSessionStore>(db);
    CallSession session;
    session.call_id = kCall;
    session.media_epoch = 1;
    ASSERT_TRUE(sessions_->UpsertSession(session));

    CallMediaKeyExchange::Transport transport;
    transport.resolve_peer_session_key = [this](const std::string&) -> Roe<ByteVector> {
      if (!peer_session_ready_) {
        return Error("no session with the peer yet");
      }
      return session_key_;
    };
    exchange_ = std::make_unique<CallMediaKeyExchange>(*sessions_, *keys_, std::move(transport));
    exchange_->SetOnEpochInstalled([this](const std::string&) { ++installed_; });
  }

  void TearDown() override {
    exchange_.reset();
    sessions_.reset();
    keys_->ClearDek();
    keys_.reset();
    std::filesystem::remove_all(dir_);
  }

  /** The coordinator's CallMediaKey for `epoch`, wrapped under the pair's session key. */
  std::string MediaKeyDetail(uint32_t epoch) {
    CallMediaKeyDetail detail;
    detail.call_id = kCall;
    detail.media_epoch = epoch;
    detail.media_key_id = "mk:" + std::to_string(epoch);
    detail.wrapped_key_b64 =
        CallMediaKeyStore::WrapKeyB64(session_key_, Bytes(static_cast<uint8_t>(0x40 + epoch)), kCall, epoch,
                                     detail.media_key_id)
            .value();
    return CallControlCodec::EncodeMediaKey(detail).value();
  }

  uint32_t SessionEpoch() { return sessions_->LoadSession(kCall).value()->media_epoch; }

  static constexpr const char* kCall = "call:rotate";
  std::filesystem::path dir_;
  std::unique_ptr<CallMediaKeyStore> keys_;
  std::unique_ptr<CallSessionStore> sessions_;
  std::unique_ptr<CallMediaKeyExchange> exchange_;
  ByteVector session_key_ = Bytes(0x10);
  bool peer_session_ready_ = false;
  int installed_ = 0;
};

// #381: a rotation's key that cannot be opened yet leaves the call on the epoch it has a key for; the
// coordinator's resend then moves it and installs the key. A resend of the current epoch installs again.
TEST_F(CallMediaKeyExchangeTest, AKeyThatFailedToOpenIsTakenFromItsResend) {
  const std::string epoch2 = MediaKeyDetail(2);
  ASSERT_TRUE(exchange_->HandleInbound(epoch2, "account:coordinator"));
  EXPECT_EQ(SessionEpoch(), 1u) << "no key for epoch 2: the session stays where its key is";
  EXPECT_EQ(installed_, 0);

  peer_session_ready_ = true;
  ASSERT_TRUE(exchange_->HandleInbound(epoch2, "account:coordinator"));
  EXPECT_EQ(SessionEpoch(), 2u);
  EXPECT_EQ(installed_, 1);
  auto current = exchange_->Current(kCall);
  ASSERT_TRUE(current);
  EXPECT_EQ(current->epoch, 2u);
  EXPECT_EQ(current->key, Bytes(0x42));

  ASSERT_TRUE(exchange_->HandleInbound(epoch2, "account:coordinator"));
  EXPECT_EQ(installed_, 2) << "the current epoch's key again installs again (idempotent)";
  ASSERT_TRUE(exchange_->HandleInbound(MediaKeyDetail(1), "account:coordinator"));
  EXPECT_EQ(SessionEpoch(), 2u) << "an older epoch never moves the call back";
  EXPECT_EQ(installed_, 2);
}

} // namespace
