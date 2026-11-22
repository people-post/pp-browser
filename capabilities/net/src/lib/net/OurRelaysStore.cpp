#include "lib/net/OurRelaysStore.h"

#include "lib/net/RelayOrigins.h"
#include "lib/people/RegistrationStatus.h"

#include "common/AtomicFileWrite.h"
#include "common/BoundedRead.h"
#include "common/Logger.h"
#include "common/PlatformLimits.h"
#include "common/ValueJson.h"
#include "foundation/error/AppError.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <utility>

namespace pbr {

namespace {

constexpr int64_t kFileVersion = 3; // 3: each relay names its relay id (the signed-format cutover)

std::filesystem::path ListPath(const std::string& profile_data_dir) {
  return std::filesystem::path(profile_data_dir) / "our_relays.json";
}

const char* StateName(const RelayRegistrationRecord::State state) {
  switch (state) {
    case RelayRegistrationRecord::State::Registered:
      return "registered";
    case RelayRegistrationRecord::State::Failed:
      return "failed";
    case RelayRegistrationRecord::State::NotRegistered:
      return "not_registered";
    case RelayRegistrationRecord::State::Expired:
      return "expired";
    case RelayRegistrationRecord::State::Unknown:
      break;
  }
  return "unknown";
}

RelayRegistrationRecord::State StateFromName(const std::string& name) {
  if (name == "registered") {
    return RelayRegistrationRecord::State::Registered;
  }
  if (name == "failed") {
    return RelayRegistrationRecord::State::Failed;
  }
  if (name == "not_registered") {
    return RelayRegistrationRecord::State::NotRegistered;
  }
  if (name == "expired") {
    return RelayRegistrationRecord::State::Expired;
  }
  return RelayRegistrationRecord::State::Unknown;
}

Object RecordToObject(const RelayRegistrationRecord& record) {
  Object out;
  out.set("state", std::string(StateName(record.state)));
  if (!record.reason.empty()) {
    out.set("reason", record.reason);
  }
  if (!record.expires_at.empty()) {
    out.set("expires_at", record.expires_at);
  }
  if (record.card_issued_at > 0) {
    out.set("card_issued_at", record.card_issued_at);
  }
  if (!record.registration_id.empty()) {
    out.set("registration_id", record.registration_id);
  }
  if (record.at_ms > 0) {
    out.set("at_ms", record.at_ms);
  }
  return out;
}

RelayRegistrationRecord RecordFromObject(const Object& object) {
  return RelayRegistrationRecord{.state = StateFromName(object.getString("state").value_or("")),
                                 .reason = object.getString("reason").value_or(""),
                                 .expires_at = object.getString("expires_at").value_or(""),
                                 .card_issued_at = object.getIf<int64_t>("card_issued_at").value_or(0),
                                 .registration_id = object.getString("registration_id").value_or(""),
                                 .at_ms = object.getIf<int64_t>("at_ms").value_or(0)};
}

Error ListError(const RelayListProblem problem) {
  return Error(RelayListProblemCode(problem));
}

std::vector<OurRelayEntry>::const_iterator Find(const OurRelaysList& list, const std::string& origin) {
  return std::find_if(list.relays.begin(), list.relays.end(),
                      [&origin](const OurRelayEntry& entry) { return SameRelay(entry.origin, origin); });
}

} // namespace

std::vector<std::string> OurRelaysList::Origins() const {
  std::vector<std::string> out;
  out.reserve(relays.size());
  for (const OurRelayEntry& entry : relays) {
    out.push_back(entry.origin);
  }
  return out;
}

std::string OurRelaysList::Preferred() const {
  return relays.empty() ? std::string() : relays.front().origin;
}

bool OurRelaysList::Lists(const std::string& origin) const {
  return Find(*this, origin) != relays.end();
}

const char* RelayListProblemCode(const RelayListProblem problem) {
  switch (problem) {
    case RelayListProblem::AlreadyListed:
      return "relay_already_listed";
    case RelayListProblem::Full:
      return "relay_list_full";
    case RelayListProblem::NotListed:
      return "relay_not_listed";
    case RelayListProblem::PreferredNotRemovable:
      return "relay_preferred_not_removable";
    case RelayListProblem::LastRelay:
      return "relay_list_last";
    case RelayListProblem::NotMainDevice:
      return "relay_list_not_main_device";
    case RelayListProblem::None:
      break;
  }
  return "";
}

RelayListProblem RelayListProblemFromCode(const std::string& code) {
  for (const RelayListProblem problem :
       {RelayListProblem::AlreadyListed, RelayListProblem::Full, RelayListProblem::NotListed,
        RelayListProblem::PreferredNotRemovable, RelayListProblem::LastRelay, RelayListProblem::NotMainDevice}) {
    if (code == RelayListProblemCode(problem)) {
      return problem;
    }
  }
  return RelayListProblem::None;
}

Roe<OurRelaysList> WithRelayAdded(const OurRelaysList& list, const std::string& base_url, const RelayId& relay_id) {
  if (list.Lists(base_url)) {
    return ListError(RelayListProblem::AlreadyListed);
  }
  if (list.relays.size() >= kMaxOurRelays) {
    return ListError(RelayListProblem::Full);
  }
  OurRelaysList next = list;
  next.relays.push_back(OurRelayEntry{.origin = base_url, .relay_id = relay_id});
  return next;
}

Roe<OurRelaysList> WithRelayRemoved(const OurRelaysList& list, const std::string& origin) {
  const auto found = Find(list, origin);
  if (found == list.relays.end()) {
    return ListError(RelayListProblem::NotListed);
  }
  if (list.relays.size() == 1) {
    return ListError(RelayListProblem::LastRelay); // mesh-only is not offered in this step (MR006)
  }
  if (found == list.relays.begin()) {
    return ListError(RelayListProblem::PreferredNotRemovable);
  }
  OurRelaysList next = list;
  next.relays.erase(next.relays.begin() + (found - list.relays.begin()));
  return next;
}

Roe<OurRelaysList> WithRelayPreferred(const OurRelaysList& list, const std::string& origin) {
  const auto found = Find(list, origin);
  if (found == list.relays.end()) {
    return ListError(RelayListProblem::NotListed);
  }
  OurRelaysList next;
  next.relays.push_back(*found);
  for (const OurRelayEntry& entry : list.relays) {
    if (!SameRelay(entry.origin, origin)) {
      next.relays.push_back(entry);
    }
  }
  return next;
}

std::string OurRelaysToJson(const OurRelaysList& list) {
  Object root;
  root.set("schema_version", kFileVersion);
  std::vector<Value> relays;
  for (const OurRelayEntry& entry : list.relays) {
    Object relay;
    relay.set("origin", entry.origin);
    if (!entry.relay_id.empty()) {
      relay.set("relay_id", entry.relay_id.ToString());
    }
    relay.set("registration", ObjectValue(RecordToObject(entry.registration)));
    relays.push_back(ObjectValue(std::move(relay)));
  }
  root.set("relays", makeArray(std::move(relays)));
  return DumpJson(root);
}

std::optional<OurRelaysList> OurRelaysFromJson(const std::string& text) {
  const auto root = TryParseObject(text);
  if (!root || root->getIf<int64_t>("schema_version").value_or(0) != kFileVersion) {
    return std::nullopt; // not ours to read (another schema, garbage)
  }
  const Array* relays = root->getArray("relays");
  if (!relays) {
    return std::nullopt;
  }
  OurRelaysList list;
  for (const Value& item : relays->elements) {
    const Object* relay = asObject(item);
    const auto origin = relay ? relay->getString("origin") : std::nullopt;
    if (!origin || origin->empty() || list.Lists(*origin) || list.relays.size() >= kMaxOurRelays) {
      continue;
    }
    const std::string named = relay->getString("relay_id").value_or("");
    const auto relay_id = RelayId::Parse(named);
    if (!named.empty() && !relay_id) {
      continue; // an address that names no relay id we could check
    }
    const Object* registration = relay->getObject("registration");
    list.relays.push_back(
        OurRelayEntry{.origin = *origin,
                      .relay_id = relay_id.value_or(RelayId{}),
                      .registration = registration ? RecordFromObject(*registration) : RelayRegistrationRecord{}});
  }
  if (list.relays.empty()) {
    return std::nullopt;
  }
  return list;
}

std::string RelayRegistrationFailureCode(const Error& error) {
  const ErrorCategory category = AppError::CategoryOf(error);
  if (category == ErrorCategory::Auth) {
    const auto code = static_cast<Err::Auth>(error.code);
    if (code == Err::Auth::ClientUpgradeRequired) {
      return kRegistrationUpgrade;
    }
    if (code == Err::Auth::RelayUpgradeRequired) {
      return kRegistrationRelayUpgrade;
    }
    if (code == Err::Auth::AccountCardRefused) {
      return kRegistrationCard;
    }
    if (code == Err::Auth::RelayIdentityMismatch || code == Err::Auth::RelayIdentityTaken ||
        code == Err::Auth::WrongAudience) {
      return kRegistrationIdentity;
    }
    return kRegistrationRefused;
  }
  if (category == ErrorCategory::Network) {
    const auto code = static_cast<Err::Network>(error.code);
    if (code == Err::Network::RelayIdentityUnavailable) {
      return kRegistrationIdentity;
    }
    return code == Err::Network::HttpRejected ? kRegistrationRefused : kRegistrationUnreachable;
  }
  return kRegistrationOther;
}

OurRelaysStore::OurRelaysStore(std::string profile_data_dir) : profile_data_dir_(std::move(profile_data_dir)) {}

bool OurRelaysFileIsNewer(const std::string& text) {
  const auto root = TryParseObject(text);
  return root && root->getIf<int64_t>("schema_version").value_or(0) > kFileVersion;
}

OurRelaysList OurRelaysStore::LoadOrSeed(const std::string& org_relay, const RelayId& org_relay_id,
                                         const std::vector<std::string>& extras, const bool bare_allowed) {
  std::lock_guard lock(mutex_);
  newer_file_ = false;
  if (!profile_data_dir_.empty()) {
    std::ifstream in(ListPath(profile_data_dir_));
    if (in) {
      // An oversized file reads as unparseable (PB-063).
      const auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
      if (auto kept = text ? OurRelaysFromJson(*text) : std::nullopt) {
        list_ = std::move(*kept);
        return list_;
      }
      // A newer app's list is the user's: never overwritten (this run uses the seed, and changes nothing).
      newer_file_ = text && OurRelaysFileIsNewer(*text);
      logging::getLogger("OurRelays").error << "our_relays.json refused ("
                                            << (newer_file_ ? "a newer schema_version: kept as it is" : "not readable")
                                            << "); the deployment's relays are used";
    }
  }
  list_ = {};
  // The extras are relay links (MR008): each names the relay id its relay must prove.
  std::vector<std::string> seeded;
  std::vector<std::pair<std::string, RelayId>> named; // by RelayOriginKey: its relay id
  for (const std::string& extra : extras) {
    const size_t hash = extra.find('#');
    const auto relay_id =
        hash == std::string::npos ? std::optional<RelayId>(RelayId{}) : RelayId::Parse(extra.substr(hash + 1));
    if (!relay_id || (relay_id->empty() && !bare_allowed)) {
      logging::getLogger("OurRelays").warning << "relay.extra_base_urls: " << extra.substr(0, hash)
                                              << " left out (its link names no relay ID)";
      continue;
    }
    seeded.push_back(extra);
    named.emplace_back(RelayOriginKey(NormalizeRegistrationBackend(extra.substr(0, hash))), *relay_id);
  }
  for (const std::string& origin : OurRelayOrigins(org_relay, seeded)) {
    RelayId relay_id = list_.relays.empty() ? org_relay_id : RelayId{};
    for (const auto& [key, id] : named) {
      if (!list_.relays.empty() && key == RelayOriginKey(origin)) {
        relay_id = id;
      }
    }
    list_.relays.push_back(OurRelayEntry{.origin = origin, .relay_id = relay_id});
  }
  if (!list_.relays.empty()) {
    (void)SaveLocked(); // the seed is the user's list from now on: config is only a new profile's default
  }
  return list_;
}

OurRelaysList OurRelaysStore::List() const {
  std::lock_guard lock(mutex_);
  return list_;
}

Roe<void> OurRelaysStore::Replace(const OurRelaysList& list) {
  std::lock_guard lock(mutex_);
  OurRelaysList next;
  for (const std::string& origin : BoundOurRelays(list.Origins())) {
    OurRelayEntry entry = *Find(list, origin);
    entry.origin = origin;
    if (const auto known = Find(list_, origin); known != list_.relays.end()) {
      // `list` is a snapshot taken a while ago (a change in progress): an answer kept since is newer than its record.
      const bool newer = known->registration.at_ms > entry.registration.at_ms;
      if (entry.registration.state == RelayRegistrationRecord::State::Unknown || newer) {
        entry.registration = known->registration;
      }
    }
    next.relays.push_back(std::move(entry));
  }
  if (next.relays.empty()) {
    return ListError(RelayListProblem::LastRelay);
  }
  const OurRelaysList previous = std::exchange(list_, std::move(next));
  if (auto saved = SaveLocked(); !saved) {
    list_ = previous;
    return saved.error();
  }
  return {};
}

void OurRelaysStore::Record(const std::string& origin, const RelayRegistrationRecord& record) {
  std::lock_guard lock(mutex_);
  const auto found = std::find_if(list_.relays.begin(), list_.relays.end(),
                                  [&origin](const OurRelayEntry& entry) { return SameRelay(entry.origin, origin); });
  if (found == list_.relays.end() || found->registration == record) {
    return;
  }
  found->registration = record;
  (void)SaveLocked();
}

Roe<void> OurRelaysStore::SaveLocked() const {
  if (newer_file_) {
    return Error("our_relays.json is a newer app's: not changed");
  }
  if (profile_data_dir_.empty()) {
    return {};
  }
  std::error_code ec;
  std::filesystem::create_directories(profile_data_dir_, ec);
  return AtomicFileWrite::Write(ListPath(profile_data_dir_).string(), OurRelaysToJson(list_));
}

} // namespace pbr
