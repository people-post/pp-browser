#include "lib/messaging/GroupLinkJoinFamily.h"

#include <type_traits>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** A join is live: an answer that waited longer than this in the relay is stale (the joiner stopped waiting). */
constexpr int64_t kJoinMaxAgeMs = 10 * 60 * 1000;

} // namespace

SystemControlFamily MakeGroupLinkJoinFamily(std::function<std::optional<std::string>()> local_account,
                                            GroupLinkJoinHandlers handlers) {
  SystemControlFamily family;
  family.name = kGroupLinkJoinFamily;
  family.owns = &group_link_join::IsJoinMessage;
  family.on_lane = &group_link_join::IsJoinMessage;
  family.lane_max_age_ms = kJoinMaxAgeMs;
  family.apply = [local_account = std::move(local_account), handlers = std::move(handlers)](
                     ThreadMessage& message, const SystemControlContext& context) -> Roe<void> {
    auto parsed = group_link_join::Parse(message);
    if (!parsed) {
      return parsed.error();
    }
    const std::optional<std::string> local = local_account ? local_account() : std::nullopt;
    const std::string& sender = context.sender_identity;
    if (!*parsed || !local || sender.empty() || sender == *local) {
      return {};
    }
    std::visit(
        [&](auto& m) {
          using T = std::decay_t<decltype(m)>;
          if constexpr (std::is_same_v<T, GroupJoinRequest>) {
            if (handlers.on_request) {
              handlers.on_request(std::move(m), sender);
            }
          } else if constexpr (std::is_same_v<T, GroupJoinPreview>) {
            if (handlers.on_preview) {
              handlers.on_preview(std::move(m), sender);
            }
          } else if constexpr (std::is_same_v<T, GroupJoinRefused>) {
            if (handlers.on_refused) {
              handlers.on_refused(std::move(m), sender);
            }
          } else if (handlers.on_confirm) {
            handlers.on_confirm(std::move(m), sender);
          }
        },
        **parsed);
    return {};
  };
  return family;
}

} // namespace pbr
