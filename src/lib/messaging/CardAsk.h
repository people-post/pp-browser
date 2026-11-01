#pragma once

#include "common/chat/SystemControl.h"
#include "common/thread/ThreadRecordTypes.h"

#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>

#include "common/PbrCompat.h"

namespace pbr {

inline constexpr const char kCardAskControlType[] = "card_ask";
inline constexpr const char kCardAskFamily[] = "card_ask";

/**
 * A receiver's ask for a newer account card (multi-device M021 d6): an envelope said its sender's list is newer
 * (`sender_card_at`) than the one kept. On the control lane, through the relay — no direct link needed (a peer's
 * privacy gate may refuse one). `to_device` (the device that sent the envelope) answers with a `device_list`
 * notice: an account copy, so its card rides it.
 */
namespace card_ask {

Roe<ThreadMessage> BuildNotice(const std::string& to_device);
/** The device asked; nullopt when the message is not an ask; an error when it is a broken one. */
Roe<std::optional<std::string>> Parse(const ThreadMessage& notice);
/** Takes asks off the control lane: `answer(to_device, asker_account)`. */
SystemControlFamily MakeFamily(std::function<void(std::string to_device, std::string asker_account)> answer);

} // namespace card_ask
} // namespace pbr
