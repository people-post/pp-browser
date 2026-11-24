// The link handshake's two QR codes (docs/contracts/LINK_DEVICE.md): binary mode, error correction M, and a version a
// phone scans from another phone's screen. The test prints each code's version (the PR and the contract quote them).

#include "foundation/crypto/LinkHandshake.h"
#include "lib/ui/QrSvg.h"

#include <gtest/gtest.h>
#include <qrcodegen.hpp>

#include <iostream>
#include <string>

namespace pbr {
namespace {

/** Phones read a version 30 code (137 modules) from a screen held 15–25 cm away; much denser is unreliable. */
constexpr int kMaxLinkQrVersion = 30;

QrShape ShapeOf(const ByteVector& payload) {
  QrShape shape;
  auto svg = QrSvgContentBinary(payload, &shape);
  EXPECT_TRUE(svg) << (svg ? "" : svg.error().message);
  return shape;
}

TEST(LinkQrTest, EachPayloadIsOneBinaryCodeAtErrorCorrectionMThatPhonesScan) {
  auto keys = LinkOfferKeys::Generate();
  ASSERT_TRUE(keys);
  const ByteVector offer = LinkOfferBytes(keys->public_key()).value();
  LinkBundle bundle;
  bundle.seed = ByteVector(32, 0x11);
  bundle.expires_at_ms = 1'700'000'000'000;
  const ByteVector reply = SealLinkReply(offer, bundle).value();
  bundle.relay = RelayAddress{.origin = "https://" + std::string(100, 'r') + ".example/api/relay", // near the bound
                              .relay_id = *RelayId::FromRaw(ByteVector(kRelayIdBytes, 0x22))};
  const ByteVector relayed = SealLinkReply(offer, bundle).value();

  const QrShape offer_shape = ShapeOf(offer);
  const QrShape reply_shape = ShapeOf(reply);
  const QrShape relayed_shape = ShapeOf(relayed);
  std::cout << "link offer: " << offer.size() << " B, QR version " << offer_shape.version << " (" << offer_shape.modules
            << " modules)\n"
            << "link reply: " << reply.size() << " B, QR version " << reply_shape.version << " (" << reply_shape.modules
            << " modules)\n"
            << "link reply with a 132-byte relay origin: " << relayed.size() << " B, QR version "
            << relayed_shape.version << " (" << relayed_shape.modules << " modules)\n";
  EXPECT_EQ(offer.size(), 1202u);
  EXPECT_EQ(reply.size(), 1u + 8u + 1089u + 8u + 1u + 24u + 50u + 16u) << "the bundle without a relay is 50 bytes";
  for (const QrShape* shape : {&offer_shape, &reply_shape, &relayed_shape}) {
    EXPECT_GT(shape->version, 0);
    EXPECT_LE(shape->version, kMaxLinkQrVersion);
  }
  // Binary mode at M: the version qrcodegen picks for those bytes at M (it may only raise the level, never lower it).
  const auto medium =
      qrcodegen::QrCode::encodeBinary(std::vector<uint8_t>(offer.begin(), offer.end()), qrcodegen::QrCode::Ecc::MEDIUM);
  EXPECT_EQ(offer_shape.version, medium.getVersion());
  EXPECT_GE(static_cast<int>(medium.getErrorCorrectionLevel()), static_cast<int>(qrcodegen::QrCode::Ecc::MEDIUM));
}

TEST(LinkQrTest, ABinaryCodeKeepsEveryByteAZeroToo) {
  const ByteVector bytes = {0x01, 0x00, 0x00, 0xff, 0x00, 0x7f};
  QrShape shape;
  ASSERT_TRUE(QrSvgContentBinary(bytes, &shape));
  EXPECT_EQ(shape.bytes, 6) << "a NUL does not end the payload (encodeText would stop there)";
}

} // namespace
} // namespace pbr
