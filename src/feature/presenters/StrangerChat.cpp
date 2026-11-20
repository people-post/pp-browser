#include "feature/presenters/StrangerChat.h"

#include "foundation/i18n/LocalizationService.h"

namespace pbr {

bool IsStrangerDirectChat(const ThreadKind kind, const PeerDisplayLabel& label) {
  return kind == ThreadKind::Direct && !label.contact_id && label.trust != PeerLabelTrust::Contact;
}

std::string StrangerChatTitle(const PeerDisplayLabel& label) {
  if (label.peer_name.empty()) {
    return Tr("chat.stranger.title_unnamed");
  }
  return Tr("chat.stranger.title", {{"name", label.peer_name}});
}

std::string StrangerBarText(const PeerDisplayLabel& label) {
  if (label.peer_name.empty()) {
    return Tr("chat.stranger.bar_unnamed");
  }
  return Tr("chat.stranger.bar", {{"name", label.peer_name}});
}

} // namespace pbr
