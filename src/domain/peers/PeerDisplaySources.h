#pragma once

#include "lib/messaging/GroupRosterStore.h"
#include "lib/people/PeerDisplayResolver.h"
#include "domain/peers/DirectoryShadowCache.h"

namespace pbr {

/** Peer naming over the directory nickname cache and (optionally) the group roster. */
inline PeerDisplaySources PeerDisplaySourcesFrom(DirectoryShadowCache& shadows, GroupRosterStore* roster = nullptr) {
  PeerDisplaySources sources;
  sources.directory = [&shadows](const std::string& identity) { return shadows.Get(identity); };
  if (roster) {
    sources.group_title = [roster](const std::string& group_id) -> std::optional<std::string> {
      auto meta = roster->LoadMetadata(group_id);
      if (meta && *meta && !(*meta)->title.empty()) {
        return (*meta)->title;
      }
      return std::nullopt;
    };
  }
  return sources;
}

} // namespace pbr
