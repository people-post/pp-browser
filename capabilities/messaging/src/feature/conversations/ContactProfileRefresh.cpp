#include "feature/conversations/ContactProfileRefresh.h"

#include "lib/people/ContactIdentity.h"

#include "common/Logger.h"

namespace pbr {

ContactProfileRefresh::ContactProfileRefresh(ContactsStore& contacts, mesh::IDirectoryClient& directory)
    : contacts_(contacts), directory_(directory) {}

bool ContactProfileRefresh::Due(const Contact& contact, const int64_t now_ms) const {
  if (!ContactAccountId(contact) && !ContactRelayUserId(contact)) {
    return false;
  }
  {
    // A lookup made lately (answered or not) is not repeated until `kStaleMs` has passed.
    std::lock_guard lock(mutex_);
    const auto attempt = last_attempt_ms_.find(contact.id);
    if (attempt != last_attempt_ms_.end() && now_ms - attempt->second < kStaleMs) {
      return false;
    }
  }
  // A contact without a relay id (added from a QR code) is asked once whatever its last answer was: the answer
  // may carry the relay id.
  return !ContactRelayUserId(contact) || now_ms - contact.remote.fetched_at >= kStaleMs;
}

bool ContactProfileRefresh::NeedsRefresh(const std::string& contact_id, const int64_t now_ms) const {
  auto found = contacts_.Get(contact_id);
  if (!found || !found->has_value()) {
    return false;
  }
  return Due(**found, now_ms);
}

ContactProfileRefresh::Result ContactProfileRefresh::RefreshIfStale(const std::string& contact_id,
                                                                    const int64_t now_ms) {
  Result result;
  auto found = contacts_.Get(contact_id);
  if (!found || !found->has_value() || !Due(**found, now_ms)) {
    return result;
  }
  const auto account_id = ContactAccountId(**found);
  const auto relay_id = ContactRelayUserId(**found);
  {
    std::lock_guard lock(mutex_);
    if (!in_flight_.insert(contact_id).second) {
      return result;
    }
  }
  result.looked_up = true;
  auto hit = account_id ? directory_.LookupByAccount(*account_id) : directory_.LookupRelayUser(*relay_id);
  if (hit) {
    // A contact without a relay id takes it from an answer that has one (the whole snapshot, account checked);
    // otherwise only the nickname and photo are updated.
    auto applied = !relay_id && PrimaryRelayIdFromHit(*hit)
                       ? contacts_.ApplyRemoteSnapshot(contact_id, *hit, now_ms, account_id)
                       : contacts_.ApplyProfileRefresh(contact_id, *hit, now_ms);
    if (applied) {
      result.updated = std::move(*applied);
    } else {
      // No ids or names in the log line: the reason is all that is needed.
      logging::getLogger("ContactProfileRefresh").warning << "profile refresh skipped: " << applied.error().message;
    }
  }
  std::lock_guard lock(mutex_);
  last_attempt_ms_[contact_id] = now_ms;
  in_flight_.erase(contact_id);
  return result;
}

} // namespace pbr
