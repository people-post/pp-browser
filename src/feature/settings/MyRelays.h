#pragma once

#include "feature/settings/SettingsPortsViews.h"
#include "lib/net/OurRelaysStore.h"
#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What the page knows of one relay of ours (the app maps the messaging facade's status into it). */
struct MyRelayFacts {
  enum class Reach { Unknown, Ok, Failed };
  std::string origin;
  RelayRegistrationRecord registration;
  Reach reach = Reach::Unknown;
  int64_t reach_at_ms = 0;
  /** The PeerId its address names (its link is `origin#peer_id`, MR008); empty: none known. */
  std::string peer_id;
  /** Its identity answer names another PeerId than its address (MR008): not used. */
  bool identity_mismatch = false;
};

/**
 * Me → My relays (multi-relay R3, MR006, MR008): `relays` preferred first, each with its registration and its
 * last poll — or, when its identity answer does not match its address, that it is not used — and its relay link (to
 * copy, on every device). On the main device (`editable`) every relay but the preferred one may be made preferred (not
 * a mismatched one) or removed, and another added while there are fewer than four; elsewhere the list only shows.
 * `org_identity_mismatch`: the org's origin, not on the list, whose identity answer does not match (its line shows on
 * every device).
 */
MyRelaysView MyRelaysViewFor(const std::vector<MyRelayFacts>& relays, bool editable,
                             const std::string& org_identity_mismatch = "");

/** A relay's host (and port) from its base URL: `https://relay.example.org/api/relay` → `relay.example.org`. */
std::string MyRelayLabel(const std::string& origin);

/** What a refused change says (the codes of OurRelaysStore / MyRelaysWorkflow), else the error's own message. */
std::string MyRelaysFailureText(const Error& error);

} // namespace pbr
