#pragma once

#include "feature/presenters/MessageResend.h"
#include "lib/delivery/DeliveryNotice.h"

#include <optional>
#include <string>

namespace pbr {

/** What the user reads for a delivery notice (a toast), in their language. */
std::string DeliveryNoticeText(const DeliveryNotice& notice);

/**
 * The transcript line under a message that did not go out, and under a group message some members missed. `advice`
 * (how a tap on the red mark goes on) picks the words of "the server can't find them".
 */
std::string SendFailureText(SendFailure failure, SendAdvice advice = SendAdvice::None);
std::string GroupMembersMissedText();
/**
 * The pill a tap on a message's red mark shows when it is not sent again (#210): `reason`'s words, else "This message
 * wasn't sent…"; for a picture, file, voice message or card without a reason, that those cannot be re-sent with a tap;
 * for a private chat or an "@ai" question, the reason (when known) and then that it cannot be re-sent with a tap.
 */
std::string SendFailureReasonText(const std::optional<SendFailure>& reason,
                                  SendMarkTap::Refusal refusal = SendMarkTap::Refusal::None,
                                  SendAdvice advice = SendAdvice::None);

/** The transcript line for a message from `peer` that could not be read. */
std::string ReceiveFailureText(ReceiveFailure reason, const std::string& peer, const std::string& family);

} // namespace pbr
