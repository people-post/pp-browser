#pragma once

#include "lib/messaging/GroupLinkJoin.h"

#include "common/chat/SystemControl.h"

#include <functional>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

inline constexpr const char kGroupLinkJoinFamily[] = "group join";

/** Where each of the four join messages goes, with its sender's account (the family only routes; all off the ingest). */
struct GroupLinkJoinHandlers {
  /** A stranger (or anyone) asks to join by one of our links: the answerer's checks run there. */
  std::function<void(GroupJoinRequest request, std::string joiner)> on_request;
  /** The answer to one of our requests. */
  std::function<void(GroupJoinPreview preview, std::string answerer)> on_preview;
  std::function<void(GroupJoinRefused refused, std::string answerer)> on_refused;
  /** A joiner confirms a preview one of our devices sent. */
  std::function<void(GroupJoinConfirm confirm, std::string joiner)> on_confirm;
};

/**
 * G021's join by link as a control-lane family, separate from the roster check's (`group roster`): its peers are
 * strangers rather than members, and nothing here may assume the sender is in a group — the handlers judge each
 * message. Never stored, applied once while fresh, an account copy carrying the sender's card (the two have never
 * met). A message from our own account is ignored.
 */
SystemControlFamily MakeGroupLinkJoinFamily(std::function<std::optional<std::string>()> local_account,
                                            GroupLinkJoinHandlers handlers);

} // namespace pbr
