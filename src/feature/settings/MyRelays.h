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
};

/**
 * Me → Network → My relays (multi-relay R3, MR006): `relays` preferred first, each with its registration and its
 * last poll. On the main device (`editable`) every relay but the preferred one may be made preferred or removed, and
 * another added while there are fewer than four; elsewhere the list only shows.
 */
MyRelaysView MyRelaysViewFor(const std::vector<MyRelayFacts>& relays, bool editable);

/** A relay's host (and port) from its base URL: `https://relay.example.org/api/relay` → `relay.example.org`. */
std::string MyRelayLabel(const std::string& origin);

/** What a refused change says (the codes of OurRelaysStore / MyRelaysWorkflow), else the error's own message. */
std::string MyRelaysFailureText(const Error& error);

} // namespace pbr
