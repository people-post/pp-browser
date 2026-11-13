// What a failed send or an unreadable message tells the user, as kinds (the GUI words them).
#include "lib/delivery/DeliveryNotice.h"
#include "lib/delivery/HttpRelayClient.h"

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

constexpr const char* kRecipientNotFoundBody = R"({"error":"Recipient not registered","code":"recipient_not_found"})";

TEST(DeliveryNoticeTest, A404WithTheRelaysRecipientNotFoundCodeReadsAsRecipientNotFound) {
  const Error error = RelaySendHttpError(404, kRecipientNotFoundBody);
  EXPECT_TRUE(IsRelayRecipientNotFound(error));
  EXPECT_EQ(AppError::CategoryOf(error), ErrorCategory::Network);
  EXPECT_EQ(error.code, static_cast<int32_t>(Err::Network::HttpNotFound)) << "#602's auto retry still sees a 404";
  EXPECT_EQ(SendFailureFromRelay(error), SendFailure::RecipientNotFound);
}

TEST(DeliveryNoticeTest, A404WithoutThatCodeStaysRejected) {
  for (const char* body : {"", "<html>Not Found</html>", R"({"error":"Recipient not registered"})",
                           R"({"code":"route_not_found"})", R"({"code":""})", R"({"code":7})", "not json"}) {
    const Error error = RelaySendHttpError(404, body);
    EXPECT_FALSE(IsRelayRecipientNotFound(error)) << body;
    EXPECT_EQ(error.code, static_cast<int32_t>(Err::Network::HttpNotFound)) << body;
    EXPECT_EQ(SendFailureFromRelay(error), SendFailure::RelayRejected) << body;
  }
  // The code on another status is not "no such recipient".
  EXPECT_FALSE(IsRelayRecipientNotFound(RelaySendHttpError(400, kRecipientNotFoundBody)));
  EXPECT_EQ(SendFailureFromRelay(RelaySendHttpError(400, kRecipientNotFoundBody)), SendFailure::RelayRejected);
  EXPECT_FALSE(IsRelayRecipientNotFound(AppError::Network(Err::Network::HttpNotFound, "404")));
}

TEST(DeliveryNoticeTest, AnUnregisteredSenderReadsAsSenderNotRegistered) {
  const Error error = RelaySendHttpError(403, R"({"error":"Relay user not registered"})");
  EXPECT_TRUE(IsRelayUserNotRegistered(error));
  EXPECT_EQ(SendFailureFromRelay(error), SendFailure::SenderNotRegistered);
  EXPECT_EQ(SendFailureFromRelay(AppError::Auth(Err::Auth::NotRegistered, "x")), SendFailure::SenderNotRegistered);
  // Another 403 is a plain refusal, as before.
  const Error other = RelaySendHttpError(403, R"({"error":"Relay registration expired"})");
  EXPECT_FALSE(IsRelayUserNotRegistered(other));
  EXPECT_EQ(SendFailureFromRelay(other), SendFailure::RelayRejected);
}

TEST(DeliveryNoticeTest, OtherRelayFailuresAreClassifiedAsBefore) {
  EXPECT_EQ(SendFailureFromRelay(RelaySendHttpError(400, "")), SendFailure::RelayRejected);
  EXPECT_EQ(SendFailureFromRelay(RelaySendHttpError(401, "")), SendFailure::RelayRejected);
  EXPECT_EQ(SendFailureFromRelay(RelaySendHttpError(500, "")), SendFailure::RelayRejected);
  EXPECT_EQ(SendFailureFromRelay(RelaySendHttpError(503, kRecipientNotFoundBody)), SendFailure::RelayRejected);
  EXPECT_EQ(SendFailureFromRelay(AppError::Auth(Err::Auth::Forbidden, "x")), SendFailure::RelayRejected);
  EXPECT_EQ(SendFailureFromRelay(AppError::Network(Err::Network::HttpRejected, "x")), SendFailure::RelayRejected);
  EXPECT_EQ(SendFailureFromRelay(AppError::Network(Err::Network::ConnectFailed, "x")), SendFailure::RelayUnreachable);
}

TEST(DeliveryNoticeTest, TheNewReasonsRoundTripAndAnUnknownWordIsNotAReason) {
  for (const SendFailure failure : {SendFailure::RecipientNotFound, SendFailure::SenderNotRegistered}) {
    const ThreadMessage row = MakeSendFailureRow("t1", "m1", SendFailureWord(failure).c_str(), 5);
    const auto reason = SendFailureRowReason(row);
    ASSERT_TRUE(reason);
    EXPECT_EQ(SendFailureFromWire(*reason), failure);
  }
  EXPECT_EQ(SendFailureWord(SendFailure::RecipientNotFound).view(), "recipient_not_found");
  EXPECT_EQ(SendFailureWord(SendFailure::SenderNotRegistered).view(), "sender_not_registered");
  // A reader that does not know a word (an older app, or a future one) falls back to the generic reason.
  EXPECT_FALSE(SendFailureFromWire("recipient_gone_from_the_future"));
  const ThreadMessage future = MakeSendFailureRow("t1", "m1", "recipient_gone_from_the_future", 5);
  ASSERT_TRUE(SendFailureRowReason(future));
  EXPECT_FALSE(SendFailureFromWire(*SendFailureRowReason(future)));
  EXPECT_EQ(SendFailureRowMessageId(future), "m1");
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
        ReceiveFailure::KeyUpdateFailed, ReceiveFailure::MembershipUpdateFailed, ReceiveFailure::ReactionRejected,
        ReceiveFailure::GroupInviteNeedsContact}) {
    EXPECT_EQ(ReceiveFailureFromWire(ReceiveFailureWire(reason)), reason) << ReceiveFailureWire(reason);
  }
  EXPECT_FALSE(ReceiveFailureFromWire("from the future"));
}

TEST(DeliveryNoticeTest, ASendFailureRowKeepsItsReasonAndNoWords) {
  const ThreadMessage row = MakeSendFailureRow("t1", "m1", SendFailureWord(SendFailure::PeerNoAnswer).c_str(), 42);
  EXPECT_EQ(row.thread_id, "t1");
  EXPECT_EQ(row.sender_contact_id, kLocalSelfContactId);
  EXPECT_EQ(row.content_type, ChatContentType::System);
  EXPECT_TRUE(row.text.empty()) << "the transcript words it, in the reader's language";
  EXPECT_EQ(row.delivery, MessageDelivery::Local);
  const auto reason = SendFailureRowReason(row);
  ASSERT_TRUE(reason);
  EXPECT_EQ(SendFailureFromWire(*reason), SendFailure::PeerNoAnswer);
}

TEST(DeliveryNoticeTest, TheRowIdIsOnePerMessageAndKind) {
  EXPECT_EQ(MakeSendFailureRow("t", "m", "relay_busy", 1).id, MakeSendFailureRow("t", "m", "relay_busy", 2).id);
  EXPECT_NE(MakeSendFailureRow("t", "m", "relay_busy", 1).id,
            MakeSendFailureRow("t", "m", kGroupMembersMissedWire, 1).id);
}

TEST(DeliveryNoticeTest, OtherMessagesAreNotSendFailureRows) {
  ThreadMessage plain;
  plain.content_type = ChatContentType::System;
  plain.text = "hello";
  EXPECT_FALSE(SendFailureRowReason(plain));
  EXPECT_FALSE(SendFailureFromWire("nonsense"));
}

} // namespace
} // namespace pbr
