#include "feature/settings/SupportThread.h"

#include "foundation/i18n/LocalizationService.h"

#include <algorithm>

namespace pbr {

Roe<void> CheckSupportConfigured(const std::optional<ClientCompatSupport>& discovery) {
  if (!discovery || !discovery->enabled || discovery->account_id.empty()) {
    return Error("Support is not available").WithUser(Tr("settings.support.unavailable"));
  }
  return {};
}

Error WithSupportWords(Error error) {
  if (error.user.empty()) {
    error.user = Tr("settings.support.unavailable");
  }
  return error;
}

std::optional<std::string> FindSupportThread(const std::vector<Thread>& threads, const std::string& contact_id) {
  for (const Thread& thread : threads) {
    const bool with_support = std::find(thread.participant_contact_ids.begin(), thread.participant_contact_ids.end(),
                                        contact_id) != thread.participant_contact_ids.end();
    if (thread.kind == ThreadKind::Direct && thread.channel == ThreadChannel::E2ePublic && with_support) {
      return thread.id;
    }
  }
  return std::nullopt;
}

} // namespace pbr
