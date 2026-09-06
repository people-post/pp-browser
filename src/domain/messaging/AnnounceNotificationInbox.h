#pragma once

#include "domain/messaging/PeerAnnounceTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pbr {

/** One Notifications-tab row for a subscribed program tip (not live_chat). */
struct AnnounceNotificationItem {
  std::string key;  // peer|topic|program
  PeerAnnounceTip tip;
  bool dismissed = false;
  bool banner_dismissed = false;
  int64_t updated_at_ms = 0;
};

/**
 * In-memory Notifications inbox for peer-announce tips.
 * live_chat tips are ignored here (they belong on the live overlay rail).
 */
class AnnounceNotificationInbox {
public:
  /** Upsert from a program tip. Returns true when the item changed. */
  bool UpsertFromTip(const PeerAnnounceTip& tip, int64_t now_ms);

  bool Dismiss(const std::string& key);
  bool DismissBanner(const std::string& key);

  std::vector<AnnounceNotificationItem> ListActive() const;
  std::vector<AnnounceNotificationItem> ListLiveBanners() const;
  std::optional<AnnounceNotificationItem> Get(const std::string& key) const;

  size_t Size() const { return items_.size(); }

  static std::string MakeKey(const PeerAnnounceTip& tip);

private:
  std::unordered_map<std::string, AnnounceNotificationItem> items_;
};

} // namespace pbr
