#include "lib/net/RelayPinStore.h"

#include "lib/net/RelayOrigins.h"

#include "common/AtomicFileWrite.h"
#include "common/BoundedRead.h"
#include "common/Logger.h"
#include "common/PlatformLimits.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

namespace pbr {

namespace {

constexpr int64_t kSchemaVersion = 3; // 3: relay ids and certificates (the signed-format cutover; nothing older read)

logging::Logger& PinLog() {
  static logging::Logger log = logging::getLogger("RelayPins");
  return log;
}

std::filesystem::path PinsPath(const std::string& profile_data_dir) {
  return std::filesystem::path(profile_data_dir) / "relay_pins.json";
}

/** A kept certificate read back: verified again (expiry aside: it is checked where the certificate is used). */
std::optional<RelayCertificate> HeldFromB64(const std::string& certificate_b64, const RelayId& relay_id) {
  RelayCertificateCheck check =
      ReceiveRelayCertificateB64(certificate_b64, relay_id, std::numeric_limits<int64_t>::min());
  if (!check.accepted()) {
    return std::nullopt;
  }
  return std::move(check.certificate);
}

} // namespace

const char* RelayMismatchName(const RelayMismatch mismatch) {
  switch (mismatch) {
    case RelayMismatch::OtherRelay:
      return "other_relay";
    case RelayMismatch::NoValidCertificate:
      return "no_valid_certificate";
    case RelayMismatch::OriginNotListed:
      return "origin_not_listed";
    case RelayMismatch::None:
      break;
  }
  return "";
}

RelayMismatch RelayMismatchFromName(const std::string& name) {
  for (const RelayMismatch mismatch :
       {RelayMismatch::OtherRelay, RelayMismatch::NoValidCertificate, RelayMismatch::OriginNotListed}) {
    if (name == RelayMismatchName(mismatch)) {
      return mismatch;
    }
  }
  return RelayMismatch::None;
}

std::string RelayPinsToJson(const RelayPinsFile& file) {
  Object root;
  root.set("schema_version", kSchemaVersion);
  std::vector<Value> rows;
  for (const RelayPin& pin : file.pins) {
    Object row;
    row.set("origin", pin.origin);
    row.set("relay_id", pin.relay_id.ToString());
    row.set("pinned_at_ms", pin.pinned_at_ms);
    if (pin.Mismatched()) {
      row.set("mismatch", std::string(RelayMismatchName(pin.mismatch)));
      row.set("mismatch_at_ms", pin.mismatch_at_ms);
      if (!pin.presented_relay_id.empty()) {
        row.set("presented_relay_id", pin.presented_relay_id.ToString());
      }
    }
    rows.push_back(ObjectValue(std::move(row)));
  }
  root.set("pins", makeArray(std::move(rows)));
  std::vector<Value> certificates;
  for (const HeldRelayCertificate& held : file.certificates) {
    Object row;
    row.set("relay_id", held.certificate.relay_id.ToString());
    row.set("sequence", static_cast<int64_t>(held.certificate.body.sequence));
    row.set("certificate_b64", held.certificate.ToBase64());
    row.set("kept_at_ms", held.kept_at_ms);
    certificates.push_back(ObjectValue(std::move(row)));
  }
  root.set("certificates", makeArray(std::move(certificates)));
  return DumpJson(root);
}

std::optional<RelayPinsFile> RelayPinsFromJson(const std::string& text) {
  const auto root = TryParseObject(text);
  if (!root || root->getIf<int64_t>("schema_version").value_or(0) != kSchemaVersion) {
    return std::nullopt;
  }
  const Array* rows = root->getArray("pins");
  const Array* certificates = root->getArray("certificates");
  if (!rows || !certificates) {
    return std::nullopt;
  }
  RelayPinsFile file;
  for (const Value& item : rows->elements) {
    const Object* row = asObject(item);
    if (!row) {
      continue;
    }
    const auto relay_id = RelayId::Parse(row->getString("relay_id").value_or(""));
    RelayPin pin{
        .origin = RelayOriginKey(row->getString("origin").value_or("")),
        .relay_id = relay_id.value_or(RelayId{}),
        .pinned_at_ms = row->getIf<int64_t>("pinned_at_ms").value_or(0),
        .mismatch = RelayMismatchFromName(row->getString("mismatch").value_or("")),
        .presented_relay_id = RelayId::Parse(row->getString("presented_relay_id").value_or("")).value_or(RelayId{}),
        .mismatch_at_ms = row->getIf<int64_t>("mismatch_at_ms").value_or(0)};
    // The rules hold on disk too: one pin per origin.
    const bool clash = std::any_of(file.pins.begin(), file.pins.end(),
                                   [&pin](const RelayPin& kept) { return kept.origin == pin.origin; });
    if (pin.origin.empty() || !relay_id || clash || file.pins.size() >= RelayPinStore::kMaxPins) {
      continue;
    }
    file.pins.push_back(std::move(pin));
  }
  for (const Value& item : certificates->elements) {
    const Object* row = asObject(item);
    const auto relay_id = row ? RelayId::Parse(row->getString("relay_id").value_or("")) : std::nullopt;
    if (!relay_id || file.certificates.size() >= RelayPinStore::kMaxCertificates) {
      continue;
    }
    const bool clash =
        std::any_of(file.certificates.begin(), file.certificates.end(),
                    [&relay_id](const HeldRelayCertificate& kept) { return kept.certificate.relay_id == *relay_id; });
    auto certificate = clash ? std::nullopt : HeldFromB64(row->getString("certificate_b64").value_or(""), *relay_id);
    if (!certificate) {
      PinLog().error << "relay_pins.json: the certificate kept for " << relay_id->ToString()
                     << " does not verify: dropped";
      continue;
    }
    file.certificates.push_back(HeldRelayCertificate{.certificate = std::move(*certificate),
                                                     .kept_at_ms = row->getIf<int64_t>("kept_at_ms").value_or(0)});
  }
  return file;
}

RelayPinStore::RelayPinStore(std::string profile_data_dir) : profile_data_dir_(std::move(profile_data_dir)) {}

void RelayPinStore::Load() {
  std::lock_guard lock(mutex_);
  pins_.clear();
  certificates_.clear();
  newer_file_ = false;
  if (profile_data_dir_.empty()) {
    return;
  }
  std::ifstream in(PinsPath(profile_data_dir_));
  if (!in) {
    return; // none yet: each is expected from its address
  }
  const auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
  auto file = text ? RelayPinsFromJson(*text) : std::nullopt;
  if (!file) {
    const auto root = text ? TryParseObject(*text) : std::nullopt;
    newer_file_ = root && root->getIf<int64_t>("schema_version").value_or(0) > kSchemaVersion;
    PinLog().error << "relay_pins.json refused ("
                   << (newer_file_ ? "a newer schema_version: kept as it is, pins held in memory only"
                                   : "another schema_version, or not JSON")
                   << "): the relays are expected again from their addresses";
    return;
  }
  pins_ = std::move(file->pins);
  certificates_ = std::move(file->certificates);
}

void RelayPinStore::SetKeep(std::function<bool(const std::string& origin)> keep) {
  std::lock_guard lock(mutex_);
  keep_ = std::move(keep);
}

std::vector<RelayPin>::iterator RelayPinStore::FindLocked(const std::string& origin) {
  const std::string key = RelayOriginKey(origin);
  return std::find_if(pins_.begin(), pins_.end(), [&key](const RelayPin& pin) { return pin.origin == key; });
}

std::vector<RelayPin>::const_iterator RelayPinStore::FindLocked(const std::string& origin) const {
  const std::string key = RelayOriginKey(origin);
  return std::find_if(pins_.begin(), pins_.end(), [&key](const RelayPin& pin) { return pin.origin == key; });
}

std::optional<RelayPin> RelayPinStore::PinFor(const std::string& origin) const {
  std::lock_guard lock(mutex_);
  const auto found = FindLocked(origin);
  return found == pins_.end() ? std::nullopt : std::optional<RelayPin>(*found);
}

std::vector<std::string> RelayPinStore::OriginsOf(const RelayId& relay_id) const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> origins;
  for (const RelayPin& pin : pins_) {
    if (pin.relay_id == relay_id) {
      origins.push_back(pin.origin);
    }
  }
  return origins;
}

std::vector<RelayPin> RelayPinStore::All() const {
  std::lock_guard lock(mutex_);
  return pins_;
}

bool RelayPinStore::AddLocked(RelayPin pin) {
  if (pins_.size() >= kMaxPins) {
    // The oldest pin of a relay no one keeps (removed from our list since): pinned again by its address if it comes
    // back.
    auto oldest = pins_.end();
    for (auto it = pins_.begin(); it != pins_.end(); ++it) {
      if ((!keep_ || !keep_(it->origin)) && (oldest == pins_.end() || it->pinned_at_ms < oldest->pinned_at_ms)) {
        oldest = it;
      }
    }
    if (oldest == pins_.end()) {
      PinLog().warning << "relay pins full of kept relays; " << pin.origin << " not pinned";
      return false;
    }
    pins_.erase(oldest);
  }
  pins_.push_back(std::move(pin));
  return true;
}

PinOutcome RelayPinStore::Expect(const std::string& origin, const RelayId& relay_id, const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  const std::string key = RelayOriginKey(origin);
  const auto found = FindLocked(key);
  if (found != pins_.end() && found->relay_id == relay_id) {
    return PinOutcome::Same;
  }
  if (found == pins_.end()) {
    if (!AddLocked(RelayPin{.origin = key, .relay_id = relay_id, .pinned_at_ms = now_ms})) {
      return PinOutcome::Full;
    }
  } else {
    *found = RelayPin{.origin = key, .relay_id = relay_id, .pinned_at_ms = now_ms};
  }
  SaveLocked();
  return PinOutcome::Pinned;
}

bool RelayPinStore::NoteCheck(const std::string& origin, const RelayMismatch mismatch, const RelayId& presented,
                              const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  const auto found = FindLocked(origin);
  if (found == pins_.end()) {
    return false;
  }
  if (found->mismatch == mismatch && found->presented_relay_id == presented) {
    return false; // seen so already (a restart, another pass): said once
  }
  found->mismatch = mismatch;
  found->presented_relay_id = mismatch == RelayMismatch::None ? RelayId{} : presented;
  found->mismatch_at_ms = mismatch == RelayMismatch::None ? 0 : now_ms;
  SaveLocked();
  return true;
}

bool RelayPinStore::TakeOwnCard(const std::vector<RelayAddress>& relays, const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  bool changed = false;
  for (const RelayAddress& relay : relays) {
    const std::string key = RelayOriginKey(relay.origin);
    if (key.empty() || relay.relay_id.empty()) {
      continue;
    }
    const auto found = FindLocked(key);
    if (found != pins_.end() && found->relay_id == relay.relay_id) {
      continue;
    }
    // Our own account's word: this origin's pin follows the card.
    std::erase_if(pins_, [&key](const RelayPin& pin) { return pin.origin == key; });
    changed = true; // a pin that disagreed went, whether or not this one fits
    (void)AddLocked(RelayPin{.origin = key, .relay_id = relay.relay_id, .pinned_at_ms = now_ms});
  }
  if (changed) {
    SaveLocked();
  }
  return changed;
}

void RelayPinStore::Drop(const std::string& origin) {
  std::lock_guard lock(mutex_);
  const auto found = FindLocked(origin);
  if (found == pins_.end()) {
    return;
  }
  pins_.erase(found);
  SaveLocked();
}

std::optional<RelayCertificate> RelayPinStore::CertificateFor(const RelayId& relay_id) const {
  std::lock_guard lock(mutex_);
  for (const HeldRelayCertificate& held : certificates_) {
    if (held.certificate.relay_id == relay_id) {
      return held.certificate;
    }
  }
  return std::nullopt;
}

RelayCertificateOffer RelayPinStore::Offer(const RelayCertificate& certificate, const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  const auto found = std::find_if(
      certificates_.begin(), certificates_.end(),
      [&certificate](const HeldRelayCertificate& held) { return held.certificate.relay_id == certificate.relay_id; });
  const RelayCertificateOffer offer =
      CompareRelayCertificates(found == certificates_.end() ? nullptr : &found->certificate, certificate);
  if (offer != RelayCertificateOffer::Adopted) {
    return offer;
  }
  if (found != certificates_.end()) {
    *found = HeldRelayCertificate{.certificate = certificate, .kept_at_ms = now_ms};
    SaveLocked();
    return offer;
  }
  if (certificates_.size() >= kMaxCertificates) {
    // The oldest certificate of a relay no pin names (a contact's relay): fetched again when it is needed.
    auto oldest = certificates_.end();
    for (auto it = certificates_.begin(); it != certificates_.end(); ++it) {
      const bool named = std::any_of(pins_.begin(), pins_.end(),
                                     [&it](const RelayPin& pin) { return pin.relay_id == it->certificate.relay_id; });
      if (!named && (oldest == certificates_.end() || it->kept_at_ms < oldest->kept_at_ms)) {
        oldest = it;
      }
    }
    if (oldest == certificates_.end()) {
      oldest = std::min_element(
          certificates_.begin(), certificates_.end(),
          [](const HeldRelayCertificate& a, const HeldRelayCertificate& b) { return a.kept_at_ms < b.kept_at_ms; });
    }
    certificates_.erase(oldest);
  }
  certificates_.push_back(HeldRelayCertificate{.certificate = certificate, .kept_at_ms = now_ms});
  SaveLocked();
  return offer;
}

void RelayPinStore::SaveLocked() const {
  if (profile_data_dir_.empty() || newer_file_) {
    return; // a newer app's file is never overwritten
  }
  std::error_code ec;
  std::filesystem::create_directories(profile_data_dir_, ec);
  const std::string text = RelayPinsToJson(RelayPinsFile{.pins = pins_, .certificates = certificates_});
  if (auto written = AtomicFileWrite::Write(PinsPath(profile_data_dir_).string(), text); !written) {
    PinLog().error << "relay_pins.json not written: " << written.error().message;
  }
}

} // namespace pbr
