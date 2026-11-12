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

    exchange_ = std::make_unique<CallMediaKeyExchange>(*sessions_, *keys_, CallMediaKeyExchange::Transport{});
  }

  void TearDown() override {
    exchange_.reset();
    sessions_.reset();
    keys_->ClearDek();
    keys_.reset();
    std::filesystem::remove_all(dir_);
  }

  static constexpr const char* kCall = "call:rotate";
  std::filesystem::path dir_;
  std::unique_ptr<InMemoryCallMediaKeyStore> keys_;
  std::unique_ptr<InMemoryCallSessionStore> sessions_;
  std::unique_ptr<CallMediaKeyExchange> exchange_;
};

/** A CallMediaKey for `epoch`: the key itself (M024). */
std::string MediaKeyDetail(const char* call, uint32_t epoch, const ByteVector& key) {
  CallMediaKeyDetail detail;
  detail.call_id = call;
  detail.media_epoch = epoch;
  detail.media_key_id = "mk:" + std::to_string(epoch);
  detail.media_key_b64 = Base64Encode(key);
  return CallControlCodec::EncodeMediaKey(detail).value();
}

// M024: the key itself rides the lane copy sealed for this device; it is taken and stored. Moving the call to its
// epoch is the workflow's (the rows' one writer, #381): the exchange only says which key it took.
TEST_F(CallMediaKeyExchangeTest, TheKeyItselfIsTaken) {
  auto taken = exchange_->HandleInbound(MediaKeyDetail(kCall, 2, Bytes(0x77)), "account:caller");
  ASSERT_TRUE(taken);
  ASSERT_TRUE(*taken);
  EXPECT_EQ((*taken)->call_id, kCall);
  EXPECT_EQ((*taken)->epoch, 2u);
  EXPECT_EQ((*taken)->key_id, "mk:2");
  EXPECT_EQ(keys_->LoadEpochKey(kCall, 2).value().value(), Bytes(0x77));
  EXPECT_EQ(sessions_->LoadSession(kCall).value()->media_epoch, 1u) << "the exchange never writes the row";
}

// A CallMediaKey that carries no key is not taken: nothing stored, the call stays on the epoch it has a key for.
TEST_F(CallMediaKeyExchangeTest, AMessageWithNoKeyIsNotTaken) {
  CallMediaKeyDetail detail;
  detail.call_id = kCall;
  detail.media_epoch = 2;
  detail.media_key_id = "mk:2";
  auto taken = exchange_->HandleInbound(CallControlCodec::EncodeMediaKey(detail).value(), "account:caller");
  ASSERT_TRUE(taken);
  EXPECT_FALSE(*taken);
  EXPECT_FALSE(keys_->LoadEpochKey(kCall, 2).value());
}

// A key of the wrong size is refused, not stored, and the call stays on the epoch it has a key for (#381).
TEST_F(CallMediaKeyExchangeTest, AMalformedKeyIsNotTaken) {
  auto taken = exchange_->HandleInbound(MediaKeyDetail(kCall, 2, Bytes(0x10, 16)), "account:caller");
  ASSERT_TRUE(taken);
  EXPECT_FALSE(*taken);
  EXPECT_FALSE(keys_->LoadEpochKey(kCall, 2).value());
  EXPECT_EQ(sessions_->LoadSession(kCall).value()->media_epoch, 1u);
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
