// What a failed send or an unreadable message tells the user, as kinds (the GUI words them).
#include "lib/messaging/DeliveryNotice.h"

#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// A failed send's kind comes from the transport's code, never its text.
TEST(DeliveryNoticeTest, SendFailuresFromTheDirectSendsCode) {
  EXPECT_EQ(SendFailureFromDirect(DirectSendErr::NotStarted), SendFailure::MeshOff);
  EXPECT_EQ(SendFailureFromDirect(DirectSendErr::NoAddress), SendFailure::NoAddress);
  EXPECT_EQ(SendFailureFromDirect(DirectSendErr::DialInBackoff), SendFailure::BackingOff);
  EXPECT_EQ(SendFailureFromDirect(DirectSendErr::DialBusy), SendFailure::DialBusy);
  EXPECT_EQ(SendFailureFromDirect(DirectSendErr::NoAnswer), SendFailure::PeerNoAnswer);
  EXPECT_EQ(SendFailureFromDirect(DirectSendErr::TransportFailed), SendFailure::PeerNoAnswer);
  EXPECT_EQ(SendFailureFromDirect(DirectSendErr::ChannelFailed), SendFailure::HandshakeFailed);
  EXPECT_EQ(SendFailureFromDirect(DirectSendErr::NotConfirmed), SendFailure::DirectUnconfirmed);
  EXPECT_EQ(SendFailureFromDirect(DirectSendErr::Generic), SendFailure::Unknown);
}

TEST(DeliveryNoticeTest, SendFailuresFromTheRelaysAppErrorCode) {
  EXPECT_EQ(SendFailureFromRelay(AppError::Config(Err::Config::MissingKey, "x")), SendFailure::RelayNotConfigured);
  EXPECT_EQ(SendFailureFromRelay(AppError::Network(Err::Network::DnsFailed, "x")), SendFailure::RelayUnreachable);
  EXPECT_EQ(SendFailureFromRelay(AppError::Network(Err::Network::Timeout, "x")), SendFailure::RelayUnreachable);
  EXPECT_EQ(SendFailureFromRelay(AppError::Network(Err::Network::HttpError, "x")), SendFailure::RelayRejected);
  EXPECT_EQ(SendFailureFromRelay(Error("text only")), SendFailure::Unknown) << "no code, no guess from text";
}

TEST(DeliveryNoticeTest, ARateLimitReadsAsBusyForATrustedPeer) {
  const SendFailure limited = SendFailureFromRelay(AppError::Auth(Err::Auth::RateLimited, "Relay rate limit exceeded"));
  EXPECT_EQ(limited, SendFailure::RelayRateLimited);
  EXPECT_EQ(SendFailureForThread(limited, /*trusted_peer=*/false), SendFailure::RelayRateLimited);
  EXPECT_EQ(SendFailureForThread(limited, /*trusted_peer=*/true), SendFailure::RelayBusy);
  EXPECT_EQ(SendFailureForThread(SendFailure::PeerNoAnswer, true), SendFailure::PeerNoAnswer);
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
