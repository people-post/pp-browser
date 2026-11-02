#include "lib/calls/testing/InMemoryCallStores.h"
#include "lib/calls/CallMediaKeyExchange.h"

#include "lib/calls/CallControlCodec.h"
#include "lib/calls/ICallMediaKeyStore.h"
#include "lib/calls/ICallSessionStore.h"
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
    keys_ = std::make_unique<InMemoryCallMediaKeyStore>();
    ASSERT_TRUE(keys_->SetDek(Bytes(0xa0, kDataEncryptionKeySize)));
    sessions_ = std::make_unique<InMemoryCallSessionStore>();
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
        ICallMediaKeyStore::WrapKeyB64(session_key_, Bytes(static_cast<uint8_t>(0x40 + epoch)), kCall, epoch,
                                     detail.media_key_id)
            .value();
    return CallControlCodec::EncodeMediaKey(detail).value();
  }

  static constexpr const char* kCall = "call:rotate";
  std::filesystem::path dir_;
  std::unique_ptr<InMemoryCallMediaKeyStore> keys_;
  std::unique_ptr<InMemoryCallSessionStore> sessions_;
  std::unique_ptr<CallMediaKeyExchange> exchange_;
  ByteVector session_key_ = Bytes(0x10);
  bool peer_session_ready_ = false;
};

// #381: a rotation's key that cannot be opened yet is not taken (the call stays on the epoch it has a key
// for); the coordinator's resend is taken, and its key stored. Moving the call is the workflow's (the rows'
// one writer): the exchange only says which key it took.
TEST_F(CallMediaKeyExchangeTest, AKeyThatFailedToOpenIsTakenFromItsResend) {
  const std::string epoch2 = MediaKeyDetail(2);
  auto first = exchange_->HandleInbound(epoch2, "account:coordinator");
  ASSERT_TRUE(first);
  EXPECT_FALSE(*first) << "no key for epoch 2 yet: nothing taken";
  EXPECT_FALSE(keys_->LoadEpochKey(kCall, 2).value());

  peer_session_ready_ = true;
  auto resend = exchange_->HandleInbound(epoch2, "account:coordinator");
  ASSERT_TRUE(resend);
  ASSERT_TRUE(*resend);
  EXPECT_EQ((*resend)->call_id, kCall);
  EXPECT_EQ((*resend)->epoch, 2u);
  EXPECT_EQ((*resend)->key_id, "mk:2");
  auto stored = keys_->LoadEpochKey(kCall, 2);
  ASSERT_TRUE(stored && *stored);
  EXPECT_EQ(**stored, Bytes(0x42));
  EXPECT_EQ(sessions_->LoadSession(kCall).value()->media_epoch, 1u) << "the exchange never writes the row";
}

// Epochs only move forward: a newer key moves the call, the current one again installs again (a member
// that missed the rotation catches up), an older one is not the key now.
TEST_F(CallMediaKeyExchangeTest, EpochsOnlyMoveForward) {
  using Step = CallMediaKeyExchange::EpochStep;
  EXPECT_EQ(CallMediaKeyExchange::StepFor(1, 2), Step::Advance);
  EXPECT_EQ(CallMediaKeyExchange::StepFor(2, 2), Step::Reinstall);
  EXPECT_EQ(CallMediaKeyExchange::StepFor(2, 1), Step::Ignore);
}

} // namespace
