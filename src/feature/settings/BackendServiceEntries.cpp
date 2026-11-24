#include "feature/settings/BackendServiceEntries.h"

#include "foundation/i18n/LocalizationService.h"

#include <utility>

namespace pbr {

std::vector<BackendServiceEntry> BackendServiceEntries(const BackendTable& table, const bool own_feed_set) {
  const auto entry = [](std::string id, std::string title, const bool named, std::string reason) {
    return BackendServiceEntry{.id = std::move(id),
                               .title = std::move(title),
                               .available = named,
                               .reason = named ? std::string() : std::move(reason)};
  };
  return {
      entry("ai", Tr("settings.service.ai.title"), !table.llm_gateway_url.empty(), Tr("settings.service.ai.missing")),
      entry("ai_key", Tr("settings.service.ai_key.title"), !table.llm_token_url.empty(),
            Tr("settings.service.ai_key.missing")),
      entry("people", Tr("settings.service.people.title"), !table.directory_providers.empty(),
            Tr("settings.service.people.missing")),
      entry("feeds", Tr("settings.service.feeds.title"), !table.promoted_mcp_url.empty() || own_feed_set,
            Tr("settings.service.feeds.missing")),
      entry("crash_reports", Tr("settings.service.crash_reports.title"), !table.crash_intake_url.empty(),
            Tr("settings.service.crash_reports.missing")),
      entry("account_backup", Tr("settings.service.account_backup.title"), !table.account_locker_url.empty(),
            Tr("settings.service.account_backup.missing")),
      entry("support_info", Tr("settings.service.support_info.title"), !table.client_compat_url.empty(),
            Tr("settings.service.support_info.missing")),
  };
}

} // namespace pbr
