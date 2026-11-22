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

/** A failed registration's reason code (RelayRegistrationFailureCode), in the person's words. */
std::string FailedText(const std::string& reason) {
  static const std::map<std::string, std::string> kKeys = {
      {kRegistrationUnreachable, "settings.relays.state.failed.unreachable"},
      {kRegistrationRefused, "settings.relays.state.failed.refused"},
      {kRegistrationIdentity, "settings.relays.state.failed.identity"},
      {kRegistrationUpgrade, "settings.relays.state.failed.upgrade"},
      {kRegistrationRelayUpgrade, "settings.relays.state.failed.relay_upgrade"},
      {kRegistrationCard, "settings.relays.state.failed.card"},
  };
  const auto key = kKeys.find(reason);
  return Tr(key == kKeys.end() ? "settings.relays.state.failed" : key->second);
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
      row.state = FailedText(registration.reason);
      return;
    case RelayRegistrationRecord::State::Expired:
      row.state_kind = "error";
      row.state = Tr("settings.relays.state.expired", {{"date", Day(registration.expires_at)}});
      return;
    case RelayRegistrationRecord::State::NotRegistered:
      row.state_kind = row.preferred ? "error" : "pending";
      row.state = Tr(row.preferred ? "settings.relays.state.not_registered" : "settings.relays.state.after_preferred");
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

MyRelaysView MyRelaysViewFor(const std::vector<MyRelayFacts>& relays, const bool editable,
                             const std::string& org_identity_mismatch) {
  MyRelaysView view;
  view.editable = editable;
  view.can_add = editable && relays.size() < kMaxOurRelays;
  view.org_identity_mismatch = org_identity_mismatch;
  for (size_t i = 0; i < relays.size(); ++i) {
    const MyRelayFacts& facts = relays[i];
    MyRelayRowView row{.origin = facts.origin, .label = MyRelayLabel(facts.origin), .preferred = i == 0};
    if (facts.identity_mismatch) {
      // Not used (MR008): not polled, registered on or sent to, so no registration or poll to show.
      row.state_kind = "error";
      row.state = Tr("settings.relays.state.identity_mismatch");
      row.identity_mismatch = true;
    } else {
      FillState(row, facts.registration);
      FillReach(row, facts);
      row.link = RelayLink(facts.origin, facts.relay_id);
      if (facts.relay_id.empty()) {
        row.link.clear(); // a bare origin (a developer build) has no link to give
      }
    }
    row.can_make_preferred = editable && i > 0 && !facts.identity_mismatch;
    row.can_remove = editable && i > 0; // the preferred one stays until another is preferred; never the last
    view.relays.push_back(std::move(row));
  }
  return view;
}

std::string MyRelaysFailureText(const Error& error) {
  // relay_identity_taken|<host>: the host that holds the PeerId the link names (MR008).
  if (const size_t bar = error.message.find('|'); bar != std::string::npos) {
    const std::string code = error.message.substr(0, bar);
    const std::string host = error.message.substr(bar + 1);
    if (code == "relay_identity_taken") {
      return Tr("settings.relays.error.identity_taken", {{"host", host}});
    }
    if (code == "relay_identity_taken_known") {
      return Tr("settings.relays.error.identity_taken_known", {{"host", host}});
    }
  }
  static const std::map<std::string, std::string> kKeys = {
      {"relay_origin_empty", "settings.relays.error.empty"},
      {"relay_origin_not_a_url", "settings.relays.error.not_a_url"},
      {"relay_origin_not_https", "settings.relays.error.not_https"},
      {"relay_origin_has_path", "settings.relays.error.has_path"},
      {"relay_origin_has_query", "settings.relays.error.has_path"},
      {"relay_origin_too_long", "settings.relays.error.too_long"},
      {"relay_origin_bad_relay_id", "settings.relays.error.bad_relay_id"},
      {"relay_link_no_id", "settings.relays.error.no_relay_id"},
      {"relay_origin_local", "settings.relays.error.local"},
      {"relay_unreachable", "settings.relays.error.unreachable"},
      {"relay_already_listed", "settings.relays.error.already_listed"},
      {"relay_list_full", "settings.relays.error.full"},
      {"relay_preferred_not_removable", "settings.relays.error.preferred"},
      {"relay_list_last", "settings.relays.error.last"},
      {"relay_list_not_main_device", "settings.relays.not_main"},
      {"relay_list_not_applied", "settings.relays.error.not_applied"},
      {"relay_identity_unavailable", "settings.relays.error.identity_unavailable"},
      {"relay_identity_mismatch", "settings.relays.error.identity_mismatch"},
  };
  const auto key = kKeys.find(error.message);
  return key == kKeys.end() ? error.message : Tr(key->second);
}

} // namespace pbr
