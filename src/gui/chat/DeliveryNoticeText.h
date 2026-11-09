#pragma once

#include "lib/delivery/DeliveryNotice.h"

#include <string>

namespace pbr {

/** What the user reads for a delivery notice (a toast), in their language. */
std::string DeliveryNoticeText(const DeliveryNotice& notice);

/** The transcript line under a message that did not go out, and under a group message some members missed. */
std::string SendFailureText(SendFailure failure);
std::string GroupMembersMissedText();

/** The transcript line for a message from `peer` that could not be read. */
std::string ReceiveFailureText(ReceiveFailure reason, const std::string& peer, const std::string& family);

} // namespace pbr
