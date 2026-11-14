// What a failed send or an unreadable message tells the user, as kinds (the GUI words them).
#include "lib/delivery/DeliveryNotice.h"
#include "lib/delivery/HttpRelayClient.h"

#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

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

TEST(DeliveryNoticeTest, ARecipientTheRelayDoesNotKnowReadsAsNotOnTheRelay) {
  EXPECT_EQ(SendFailureFromRelay(AppError::Network(Err::Network::RecipientNotRegistered, "x")),
            SendFailure::RecipientNotOnRelay);
}

TEST(DeliveryNoticeTest, APollsNotRegisteredReadsAsOurRegistrationGone) {
  const Error error = RelayPollHttpError(403, R"({"error":"Account not registered","code":"not_registered"})");
  EXPECT_TRUE(IsRelayUserNotRegistered(error));
  EXPECT_EQ(SendFailureFromRelay(error), SendFailure::SenderNotRegistered);
  // Another 403, or that code on another status, is a plain refusal.
  for (const auto& [status, body] : std::vector<std::pair<long, std::string>>{
           {403, R"({"error":"Relay user not registered"})"},
           {403, R"({"error":"Registration expired","code":"registration_expired"})"},
           {404, R"({"error":"Account not registered","code":"not_registered"})"},
           {403, "not json"}}) {
    const Error other = RelayPollHttpError(status, body);
    EXPECT_FALSE(IsRelayUserNotRegistered(other)) << status << " " << body;
    EXPECT_EQ(SendFailureFromRelay(other), SendFailure::RelayRejected) << status << " " << body;
  }
}

TEST(DeliveryNoticeTest, OtherRelayFailuresAreClassifiedAsBefore) {
  EXPECT_EQ(SendFailureFromRelay(RelayPollHttpError(400, "")), SendFailure::RelayRejected);
  EXPECT_EQ(SendFailureFromRelay(RelayPollHttpError(401, "")), SendFailure::RelayRejected);
  EXPECT_EQ(SendFailureFromRelay(RelayPollHttpError(500, "")), SendFailure::RelayRejected);
  EXPECT_EQ(SendFailureFromRelay(AppError::Auth(Err::Auth::Forbidden, "x")), SendFailure::RelayRejected);
  EXPECT_EQ(SendFailureFromRelay(AppError::Network(Err::Network::HttpRejected, "x")), SendFailure::RelayRejected);
  EXPECT_EQ(SendFailureFromRelay(AppError::Network(Err::Network::ConnectFailed, "x")), SendFailure::RelayUnreachable);
}

TEST(DeliveryNoticeTest, TheNewReasonsRoundTripAndAnUnknownWordIsNotAReason) {
  for (const SendFailure failure : {SendFailure::RecipientNotOnRelay, SendFailure::SenderNotRegistered}) {
    const ThreadMessage row = MakeSendFailureRow("t1", "m1", SendFailureWord(failure).c_str(), 5);
    const auto reason = SendFailureRowReason(row);
    ASSERT_TRUE(reason);
    EXPECT_EQ(SendFailureFromWire(*reason), failure);
  }
  EXPECT_EQ(SendFailureWord(SendFailure::RecipientNotOnRelay).view(), "recipient_not_on_relay");
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
