#include "domain/messaging/AnnounceNotificationInbox.h"

namespace pbr {

std::string AnnounceNotificationInbox::MakeKey(const PeerAnnounceTip& tip) {
  return tip.peer_id + '\x1f' + tip.topic_id + '\x1f' + tip.program_id;
}

bool AnnounceNotificationInbox::UpsertFromTip(const PeerAnnounceTip& tip, const int64_t now_ms) {
  if (!TipIsProgramKind(tip) || tip.peer_id.empty() || tip.topic_id.empty() || tip.program_id.empty()) {
    return false;
  }
  const std::string key = MakeKey(tip);
  auto it = items_.find(key);
  if (it == items_.end()) {
    AnnounceNotificationItem item;
    item.key = key;
    item.tip = tip;
    item.updated_at_ms = now_ms;
    items_.emplace(key, std::move(item));
    return true;
  }
  AnnounceNotificationItem& item = it->second;
  const PeerAnnounceTip& prev = item.tip;
  const bool newer = tip.epoch > prev.epoch || (tip.epoch == prev.epoch && tip.seq > prev.seq);
  if (!newer) {
    return false;
  }
  const bool was_live = prev.state == PeerAnnounceState::Live;
  item.tip = tip;
  item.updated_at_ms = now_ms;
  item.dismissed = false;
  if (tip.state == PeerAnnounceState::Live && (!was_live || tip.epoch != prev.epoch)) {
    item.banner_dismissed = false;
  }
  if (tip.state != PeerAnnounceState::Live) {
    item.banner_dismissed = true;
  }
  return true;
}

bool AnnounceNotificationInbox::Dismiss(const std::string& key) {
  const auto it = items_.find(key);
  if (it == items_.end()) {
    return false;
  }
  it->second.dismissed = true;
  it->second.banner_dismissed = true;
  return true;
}

bool AnnounceNotificationInbox::DismissBanner(const std::string& key) {
  const auto it = items_.find(key);
  if (it == items_.end()) {
    return false;
  }
  it->second.banner_dismissed = true;
  return true;
}

std::vector<AnnounceNotificationItem> AnnounceNotificationInbox::ListActive() const {
  std::vector<AnnounceNotificationItem> out;
  for (const auto& [_, item] : items_) {
    if (!item.dismissed) {
      out.push_back(item);
    }
  }
  return out;
}

std::vector<AnnounceNotificationItem> AnnounceNotificationInbox::ListLiveBanners() const {
  std::vector<AnnounceNotificationItem> out;
  for (const auto& [_, item] : items_) {
    if (!item.dismissed && !item.banner_dismissed && item.tip.state == PeerAnnounceState::Live) {
      out.push_back(item);
    }
  }
  return out;
}

std::optional<AnnounceNotificationItem> AnnounceNotificationInbox::Get(const std::string& key) const {
  const auto it = items_.find(key);
  if (it == items_.end()) {
    return std::nullopt;
  }
  return it->second;
}

} // namespace pbr
