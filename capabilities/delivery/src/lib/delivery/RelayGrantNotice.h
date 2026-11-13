#pragma once

#include "common/Error.h"
#include "common/chat/RelayGrant.h"
#include "common/chat/SystemControl.h"
#include "common/thread/ThreadRecordTypes.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * Relay grants between two accounts' apps (multi-relay MR005), on the control lane, peer to peer — as account cards
 * and their asks travel (M021 d6): the grantor sends its grant (`relay_grant`); the grantee confirms it kept it
 * (`relay_grant_ack`), or asks for one when it holds none it can use (`relay_grant_ask`).
 */
namespace relay_grant {

inline constexpr const char kFamily[] = "relay_grant";
inline constexpr const char kGrantControlType[] = "relay_grant";
inline constexpr const char kAckControlType[] = "relay_grant_ack";
inline constexpr const char kAskControlType[] = "relay_grant_ask";

enum class NoticeKind { Grant, Ack, Ask };

struct Notice {
  NoticeKind kind = NoticeKind::Ask;
  /** Grant: the grant itself. */
  std::optional<RelayGrant> grant;
  /** Ack: the `issued_at` of the grant kept. */
  int64_t issued_at = 0;
};

Roe<ThreadMessage> BuildGrant(const RelayGrant& grant);
Roe<ThreadMessage> BuildAck(int64_t issued_at);
Roe<ThreadMessage> BuildAsk();
/** The notice `message` is; nullopt when it is none of these; an error when it is a broken one. */
Roe<std::optional<Notice>> Parse(const ThreadMessage& message);
/** Takes the notices off the control lane: `on_notice(notice, sender_account)` (the envelope's verified sender). */
SystemControlFamily MakeFamily(std::function<Roe<void>(const Notice& notice, const std::string& sender)> on_notice);

} // namespace relay_grant
} // namespace pbr
