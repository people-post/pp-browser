#pragma once

#include "common/thread/ThreadRecordTypes.h"
#include "lib/people/PeerDisplayLabel.h"

#include <string>

namespace pbr {

/**
 * A one-to-one chat with someone who is not a contact yet (the first message from a stranger): the label has no
 * contact and is not a contact's. A blocked person is a contact record, so that chat is not one.
 */
bool IsStrangerDirectChat(ThreadKind kind, const PeerDisplayLabel& label);

/** What the list row and the chat title call a stranger: "New contact (Ann)", or "New contact" with no name. */
std::string StrangerChatTitle(const PeerDisplayLabel& label);

/** The bar at the top of a stranger's chat: "Ann is not your contact yet." (Add / Block are its buttons). */
std::string StrangerBarText(const PeerDisplayLabel& label);

} // namespace pbr
