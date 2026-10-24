#include "domain/call_media/CallMediaFrameCrypto.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

TEST(CallMediaFrameCryptoTest, RoundTripAudioFrame) {
  ByteVector key(32, 0x11);
  const std::string call_id = "call-abc";
  constexpr uint32_t epoch = 1;
  constexpr uint32_t seq = 7;
  std::vector<uint8_t> opus = {0x01, 0x02, 0x03, 0x04};
  auto encrypted = EncryptCallMediaAudioFrame(key, call_id, epoch, seq, 0, opus);
  ASSERT_TRUE(encrypted);
  auto decrypted = DecryptCallMediaAudioFrame(key, call_id, epoch, *encrypted);
  ASSERT_TRUE(decrypted);
  EXPECT_EQ(*decrypted, opus);
}

} // namespace
} // namespace pbr
