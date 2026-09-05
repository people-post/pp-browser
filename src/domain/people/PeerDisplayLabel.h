#pragma once

#include "domain/people/ContactTypes.h"

#include <optional>
#include <string>

namespace pbr {

enum class PeerLabelTrust { Contact, DirectoryUnverified, RawId, Group };

struct PeerDisplayLabel {
  std::string title;
  PeerLabelTrust trust = PeerLabelTrust::RawId;
  std::optional<std::string> contact_id;
  /** Shared group title when local override is active (for subtitle). */
  std::optional<std::string> shared_title;
};

/** Contact display title: display_name, else server_nickname. */
std::string FormatContactTitle(const Contact& contact);

/** Unverified directory nickname: "~nick @relay:…". Empty nickname → empty unless id present. */
std::string FormatDirectoryTitle(const DirectoryHit& hit);

/** Shorten relay ids for UI (e.g. relay:lHaEnkO4…). */
std::string ShortRelayId(const std::string& relay_or_peer_id);

} // namespace pbr
