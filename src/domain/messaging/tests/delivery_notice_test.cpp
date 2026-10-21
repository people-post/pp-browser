// What a failed send or an unreadable message tells the user, as kinds (the GUI words them).
#include "domain/messaging/DeliveryNotice.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(DeliveryNoticeTest, SendFailuresFromTransportText) {
  EXPECT_EQ(ClassifySendFailure("amp: host not running", false), SendFailure::MeshOff);
  EXPECT_EQ(ClassifySendFailure("Invalid multiaddr: /ip4/x", false), SendFailure::BadAddress);
  EXPECT_EQ(ClassifySendFailure("peer not registered", false), SendFailure::NoAddress);
  EXPECT_EQ(ClassifySendFailure("link in backoff (3s)", false), SendFailure::BackingOff);
  EXPECT_EQ(ClassifySendFailure("Too many concurrent dials", false), SendFailure::DialBusy);
  EXPECT_EQ(ClassifySendFailure("amp link manager: dial timeout", false), SendFailure::PeerNoAnswer);
  EXPECT_EQ(ClassifySendFailure("stream open failed", false), SendFailure::HandshakeFailed);
  EXPECT_EQ(ClassifySendFailure("no ack within 4s", false), SendFailure::DirectUnconfirmed);
  EXPECT_EQ(ClassifySendFailure("Relay client not configured", false), SendFailure::RelayNotConfigured);
  EXPECT_EQ(ClassifySendFailure("something new", false), SendFailure::Unknown);
}

TEST(DeliveryNoticeTest, ARateLimitWinsAndReadsAsBusyForATrustedPeer) {
  EXPECT_EQ(ClassifySendFailure("relay: rate limit (dial timeout)", false), SendFailure::RelayRateLimited);
  EXPECT_EQ(ClassifySendFailure("relay: rate limit (dial timeout)", true), SendFailure::RelayBusy);
}

TEST(DeliveryNoticeTest, ReceiveReasonsRoundTripTheirStoredNames) {
  for (const ReceiveFailure reason :
       {ReceiveFailure::ApplyFailed, ReceiveFailure::ThreadLookupFailed, ReceiveFailure::SenderMismatch,
        ReceiveFailure::StoreError, ReceiveFailure::SignatureCheckFailed, ReceiveFailure::BadSignature,
        ReceiveFailure::MissingLocalIdentity, ReceiveFailure::MissingKey, ReceiveFailure::DecryptFailed,
        ReceiveFailure::DecodeFailed, ReceiveFailure::Unsupported, ReceiveFailure::OpenChatFailed,
        ReceiveFailure::KeyUpdateFailed, ReceiveFailure::MembershipUpdateFailed, ReceiveFailure::ReactionRejected}) {
    EXPECT_EQ(ReceiveFailureFromWire(ReceiveFailureWire(reason)), reason) << ReceiveFailureWire(reason);
  }
  EXPECT_FALSE(ReceiveFailureFromWire("from the future"));
}

} // namespace
} // namespace pbr
