#include "feature/conversations/ContactProfileRefresh.h"

#include "lib/people/ContactIdentity.h"

#include "common/Logger.h"

namespace pbr {

ContactProfileRefresh::ContactProfileRefresh(ContactsStore& contacts, mesh::IDirectoryClient& directory)
    : contacts_(contacts), directory_(directory) {}

bool ContactProfileRefresh::NeedsRefresh(const std::string& contact_id, const int64_t now_ms) const {
  auto found = contacts_.Get(contact_id);
  if (!found || !found->has_value()) {
    return false;
  }
  const Contact& contact = **found;
  if (!ContactAccountId(contact)) {
    return false;
  }
  return now_ms - contact.remote.fetched_at >= kStaleMs;
}

ContactProfileRefresh::Result ContactProfileRefresh::RefreshIfStale(const std::string& contact_id,
                                                                    const int64_t now_ms) {
  Result result;
  auto found = contacts_.Get(contact_id);
  if (!found || !found->has_value()) {
    return result;
  }
  const auto account_id = ContactAccountId(**found);
  if (!account_id || now_ms - (*found)->remote.fetched_at < kStaleMs) {
    return result;
  }
  {
    std::lock_guard lock(mutex_);
    if (!in_flight_.insert(contact_id).second) {
      return result;
    }
  }
  result.looked_up = true;
  auto hit = directory_.LookupByAccount(*account_id);
  if (hit) {
    auto applied = contacts_.ApplyProfileRefresh(contact_id, *hit, now_ms);
    if (applied) {
      result.updated = std::move(*applied);
    } else {
      // No ids or names in the log line: the reason is all that is needed.
      logging::getLogger("ContactProfileRefresh").warning << "profile refresh skipped: " << applied.error().message;
    }
  }
  std::lock_guard lock(mutex_);
  in_flight_.erase(contact_id);
  return result;
}

} // namespace pbr
