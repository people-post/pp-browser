#pragma once

#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/PlatformShare.h"
#include "gui/UserFeedback.h"

#include <ui/dom/Element.h>
#include <ui/dom/Event.h>

#include <functional>
#include <string>

namespace pbr {

/** The sentence a user sends to a friend: name and account id in plain words (`settings.profile.invite_text`). */
inline std::string AccountInviteText(const std::string& nickname, const std::string& account_id) {
  return Tr("settings.profile.invite_text",
            {{"name", nickname.empty() ? account_id : nickname}, {"id", account_id}});
}

/** The clicked button's rectangle, so the share sheet points at it (window centre when unknown). */
inline ShareAnchor ShareAnchorFromEvent(ui::Event& ev) {
  ui::Element* target = ev.GetCurrentElement();
  if (!target) {
    target = ev.GetTargetElement();
  }
  if (!target) {
    return {};
  }
  const ui::Vector2f offset = target->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f size = target->GetBox().GetSize(ui::BoxArea::Border);
  return {offset.x, offset.y, size.x, size.y};
}

/** System sheet first; where there is none (or it cannot show) copy the invite and say so. */
inline void ShareInviteOrCopy(const std::string& invite,
                              const std::function<bool(const std::string&, ShareAnchor)>& share,
                              const ShareAnchor anchor, const std::function<void(const std::string&)>& copy) {
  if (share && share(invite, anchor)) {
    return;
  }
  if (!copy) {
    return;
  }
  copy(invite);
  UserFeedback::Ok(Tr("settings.profile.invite_copied"));
}

} // namespace pbr
