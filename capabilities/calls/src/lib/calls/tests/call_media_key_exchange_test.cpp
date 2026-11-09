#include "lib/calls/testing/InMemoryCallStores.h"
#include "lib/calls/CallMediaKeyExchange.h"

#include "lib/calls/CallControlCodec.h"
#include "lib/calls/ICallMediaKeyStore.h"
#include "lib/calls/ICallSessionStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
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
      ++sessions_opened_or_used_;
      if (!peer_session_ready_) {
        return Error("no session with the peer yet");
      }
      return session_key_;
    };
    transport.held_peer_session_key = [this](const std::string&) -> Roe<ByteVector> {
      if (!peer_session_ready_) {
        return Error("no session held with the peer");
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
  /** Calls of the port that opens a session when missing: the wrap's, never the read's. */
  int sessions_opened_or_used_ = 0;
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
  EXPECT_EQ(sessions_opened_or_used_, 0) << "reading the old form never opens a session (M024)";
}

/** A CallMediaKey for `epoch` as a sender after M024 sends it: the key itself, and (the bridge) its old form. */
std::string NewFormDetail(const char* call, uint32_t epoch, const ByteVector& key, const std::string& wrapped) {
  CallMediaKeyDetail detail;
  detail.call_id = call;
  detail.media_epoch = epoch;
  detail.media_key_id = "mk:" + std::to_string(epoch);
  detail.media_key_b64 = Base64Encode(key);
  detail.wrapped_key_b64 = wrapped;
  return CallControlCodec::EncodeMediaKey(detail).value();
}

// M024: the key itself rides the lane copy sealed for this device; it is taken with no account pair's session —
// a callee whose account session went to a sibling's opener (B186) still gets it.
TEST_F(CallMediaKeyExchangeTest, TheKeyItselfIsTakenWithNoAccountSession) {
  peer_session_ready_ = false;
  auto taken = exchange_->HandleInbound(NewFormDetail(kCall, 2, Bytes(0x77), ""), "account:caller");
  ASSERT_TRUE(taken);
  ASSERT_TRUE(*taken);
  EXPECT_EQ(keys_->LoadEpochKey(kCall, 2).value().value(), Bytes(0x77));
}

// A receiver after M024 prefers the key itself: an old form under a session it does not share does not stop it.
TEST_F(CallMediaKeyExchangeTest, TheKeyItselfWinsOverAnOldFormThatDoesNotOpen) {
  peer_session_ready_ = true; // the account pair's session we hold — not the one the sender wrapped under
  const std::string wrapped = ICallMediaKeyStore::WrapKeyB64(Bytes(0x99), Bytes(0x55), kCall, 2, "mk:2").value();
  auto taken = exchange_->HandleInbound(NewFormDetail(kCall, 2, Bytes(0x55), wrapped), "account:caller");
  ASSERT_TRUE(taken);
  EXPECT_TRUE(*taken);
  EXPECT_EQ(keys_->LoadEpochKey(kCall, 2).value().value(), Bytes(0x55));
}

// For one release a sender after M024 still wraps the key under the account pair's session, the only form an app
// before it reads (an old receiver ignores `media_key_b64`).
TEST_F(CallMediaKeyExchangeTest, TheOldFormStillGoesForAnOlderApp) {
  peer_session_ready_ = true;
  const CallMediaKeyExchange::EpochKey key{1, "mk:1", Bytes(0x31)};
  const std::string wrapped = exchange_->WrapForPeer(kCall, key, "account:old");
  if (!kMediaKeyAccountWrapBridge) {
    EXPECT_TRUE(wrapped.empty()) << "the bridge is gone";
    return;
  }
  ASSERT_FALSE(wrapped.empty());
  auto old_reader = CallControlCodec::DecodeMediaKey(NewFormDetail(kCall, 1, key.key, wrapped)).value();
  old_reader.media_key_b64.clear(); // an app before M024 does not know the field
  EXPECT_EQ(ICallMediaKeyStore::UnwrapKeyB64(session_key_, old_reader.wrapped_key_b64, kCall, 1, "mk:1").value(),
            key.key);
}

// A key of the wrong size is refused, not stored.
TEST_F(CallMediaKeyExchangeTest, AMalformedKeyIsNotTaken) {
  auto taken = exchange_->HandleInbound(NewFormDetail(kCall, 2, Bytes(0x10, 16), ""), "account:caller");
  ASSERT_TRUE(taken);
  EXPECT_FALSE(*taken);
  EXPECT_FALSE(keys_->LoadEpochKey(kCall, 2).value());
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
