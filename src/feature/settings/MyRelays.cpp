#include "feature/settings/MyRelays.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/net/RelayOrigins.h"

#include <map>
#include <string_view>

namespace pbr {

namespace {

/** "2026-12-01" of an ISO-8601 time (the day is enough for a registration's end). */
std::string Day(const std::string& iso) {
  return iso.size() >= 10 ? iso.substr(0, 10) : iso;
}

void FillState(MyRelayRowView& row, const RelayRegistrationRecord& registration) {
  switch (registration.state) {
    case RelayRegistrationRecord::State::Registered:
      row.state_kind = "ok";
      row.state = registration.expires_at.empty()
                      ? Tr("settings.relays.state.registered")
                      : Tr("settings.relays.state.registered_until", {{"date", Day(registration.expires_at)}});
      return;
    case RelayRegistrationRecord::State::Failed:
      row.state_kind = "error";
      row.state = Tr("settings.relays.state.failed", {{"reason", registration.reason}});
      return;
    case RelayRegistrationRecord::State::Unknown:
      break;
  }
  row.state_kind = "pending";
  row.state = Tr("settings.relays.state.registering");
}

void FillReach(MyRelayRowView& row, const MyRelayFacts& facts) {
  switch (facts.reach) {
    case MyRelayFacts::Reach::Ok:
      row.reach = Tr("settings.relays.reach.ok");
      return;
    case MyRelayFacts::Reach::Failed:
      row.reach_failed = true;
      row.reach = Tr("settings.relays.reach.failed");
      return;
    case MyRelayFacts::Reach::Unknown:
      break;
  }
}

} // namespace

std::string MyRelayLabel(const std::string& origin) {
  std::string_view rest(origin);
  if (const size_t scheme = rest.find("://"); scheme != std::string_view::npos) {
    rest = rest.substr(scheme + 3);
  }
  return std::string(rest.substr(0, rest.find('/')));
}

MyRelaysView MyRelaysViewFor(const std::vector<MyRelayFacts>& relays, const bool editable) {
  MyRelaysView view;
  view.editable = editable;
  view.can_add = editable && relays.size() < kMaxOurRelays;
  for (size_t i = 0; i < relays.size(); ++i) {
    const MyRelayFacts& facts = relays[i];
    MyRelayRowView row{.origin = facts.origin, .label = MyRelayLabel(facts.origin), .preferred = i == 0};
    FillState(row, facts.registration);
    FillReach(row, facts);
    row.can_make_preferred = editable && i > 0;
    row.can_remove = editable && i > 0; // the preferred one stays until another is preferred; never the last
    view.relays.push_back(std::move(row));
  }
  return view;
}

std::string MyRelaysFailureText(const Error& error) {
  static const std::map<std::string, std::string> kKeys = {
      {"relay_origin_empty", "settings.relays.error.empty"},
      {"relay_origin_not_a_url", "settings.relays.error.not_a_url"},
      {"relay_origin_not_https", "settings.relays.error.not_https"},
      {"relay_origin_has_path", "settings.relays.error.has_path"},
      {"relay_origin_has_query", "settings.relays.error.has_path"},
      {"relay_origin_too_long", "settings.relays.error.too_long"},
      {"relay_unreachable", "settings.relays.error.unreachable"},
      {"relay_already_listed", "settings.relays.error.already_listed"},
      {"relay_list_full", "settings.relays.error.full"},
      {"relay_preferred_not_removable", "settings.relays.error.preferred"},
      {"relay_list_last", "settings.relays.error.last"},
      {"relay_list_not_main_device", "settings.relays.not_main"},
  };
  const auto key = kKeys.find(error.message);
  return key == kKeys.end() ? error.message : Tr(key->second);
}

} // namespace pbr
