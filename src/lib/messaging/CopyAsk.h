#pragma once

#include "common/chat/SystemControl.h"
#include "common/thread/ThreadRecordTypes.h"

#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>

#include "common/PbrCompat.h"

namespace pbr {

/** `control_type` of a missing-copy ask (multi-device M021 §9). */
inline constexpr const char kCopyAskControlType[] = "copy_ask";
inline constexpr const char kCopyAskFamily[] = "copy_ask";

/**
 * A device found a message in its account's mailbox with copies for its siblings but none for itself — the
 * sender's device did not know it yet — and asks that device for one. On the control lane to the sender's
 * account (an account copy, carrying the asker's card: the sender learns the device from it).
 */
struct CopyAsk {
  std::string message_id;
  /** The group the message went to; empty for a direct chat. */
  std::string group_id;
  /** The device that sent the message: only it answers (its siblings may hold a copy too). */
  std::string to_device;
  /** The asking device (checked against the asker's card before anything is sealed to it). */
  std::string from_device;
};

namespace copy_ask {

/** The ask as a control-lane notice (a system message, its fields in the detail). */
Roe<ThreadMessage> BuildNotice(const CopyAsk& ask);
/** The ask a notice carries; nullopt when it is not one; an error when it is a broken one. */
Roe<std::optional<CopyAsk>> Parse(const ThreadMessage& notice);
/**
 * The family that takes asks off the control lane: `answer(ask, asker_account)` for each (the sender then seals a
 * copy for the asking device, if it is the one that sent the message).
 */
SystemControlFamily MakeFamily(std::function<void(CopyAsk ask, std::string asker_account)> answer);

} // namespace copy_ask
} // namespace pbr
