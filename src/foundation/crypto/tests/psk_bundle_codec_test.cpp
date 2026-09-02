#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/PskBundleCodec.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

ByteVector TestPskBytes() {
  const auto bytes = HexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  EXPECT_TRUE(bytes);
  return *bytes;
}

} // namespace

TEST(PskBundleCodecTest, DecodeRawBase64RoundTrip) {
  const std::string b64 = Base64Encode(TestPskBytes());
  auto decoded = PskBundleCodec::DecodeRawBase64(b64);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->master_psk_b64, b64);
  EXPECT_EQ(decoded->master_psk, TestPskBytes());
}

TEST(PskBundleCodecTest, BundleSerializeParseRoundTrip) {
  PskBundleV1 bundle;
  bundle.channel = CryptoChannel::E2e;
  bundle.active_epoch = 3;
  bundle.master_psk_b64 = Base64Encode(TestPskBytes());
  RetiredPskEntry retired;
  retired.epoch = 2;
  retired.master_psk_b64 = bundle.master_psk_b64;
  retired.retired_at = 1;
  bundle.retired_epochs.push_back(retired);

  auto json = PskBundleCodec::SerializeBundle(bundle);
  ASSERT_TRUE(json);
  auto parsed = PskBundleCodec::ParseBundleJson(*json);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->active_epoch, 3u);
  EXPECT_EQ(parsed->retired_epochs.size(), 1u);
  EXPECT_EQ(parsed->retired_epochs.front().epoch, 2u);
}

TEST(PskBundleCodecTest, CapRetiredTailKeepsMostRecent) {
  std::vector<RetiredPskEntry> retired;
  for (uint32_t epoch = 1; epoch <= 10; ++epoch) {
    RetiredPskEntry entry;
    entry.epoch = epoch;
    entry.master_psk_b64 = Base64Encode(TestPskBytes());
    retired.push_back(entry);
  }
  PskBundleCodec::CapRetiredTail(retired, 11);
  EXPECT_EQ(retired.size(), kMaxRetiredPskEpochs);
  EXPECT_EQ(retired.front().epoch, 3u);
  EXPECT_EQ(retired.back().epoch, 10u);
}

} // namespace pbr
