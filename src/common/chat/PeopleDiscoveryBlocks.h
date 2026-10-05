#pragma once

#include "common/directory/DirectoryTypes.h"
#include "common/directory/IdentityTypes.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

/** Narrow contact view for people-discovery chat blocks (avoids people→messaging coupling). */
struct PeopleDiscoveryContactView {
  std::string id;
  std::string display_name;
  std::string server_nickname;
  std::vector<ContactId> ids;
};

/**
 * User-visible strings of people-discovery blocks. Defaults are English; layers that may use i18n
 * (see domain/ai/LocalizedLabels.h) fill localized text. `{count}`, `{shown}`, `{name}` are placeholders.
 */
struct PeopleDiscoveryLabels {
  std::string message = "Message";
  std::string view = "View";
  std::string add_contact = "Add contact";
  std::string in_contacts = "In contacts";
  std::string unknown_person = "Unknown person";
  std::string start_chat_with = "Start chat with {name}";
  std::string show_name = "Show {name}";
  std::string show_ids_for = "Show IDs for {name}";
  std::string add_name = "Add {name}";
  std::string refine_label = "Refine search…";
  std::string refine_message = "Search for someone more specifically by nickname or account id";
  std::string found_one = "Found 1 person on the network:";
  std::string found_many_partial = "Found {count} people — showing the first {shown}. Open the panel to pick who you mean.";
  std::string found_many = "Found {count} people on the network:";
  std::string results_title = "Search results";
  std::string results_title_partial = "Search results (partial)";
  std::string local_one = "1 local contact:";
  std::string local_many_partial = "{count} local contacts — showing the first {shown}.";
  std::string local_many = "Your local contacts ({count}):";
  std::string contacts_title = "Contacts";
  std::string no_people = "No people found. Try a different name, nickname, or account id.";
};

/** Optional knobs when building people-discovery long_list blocks. */
struct PeopleDiscoveryBuildOptions {
  PeopleDiscoveryLabels labels;
  /** Identity values (account / relay / peer ids) already in local contacts. */
  std::unordered_set<std::string> known_local_identity_values;
  /** Local user identities — matching directory hits are omitted entirely. */
  std::unordered_set<std::string> self_identity_values;
  /** Cap visible directory / contact rows; remainder noted in footer. */
  size_t max_visible_items = 10;
};

/** account_id / relay_user_id / peer_id from local identity (non-empty only). */
std::unordered_set<std::string> SelfIdentityValuesFromLocal(const LocalIdentity& identity);

/** True when hit account/ids intersect `identities`. */
bool DirectoryHitMatchesIdentities(const DirectoryHit& hit,
                                   const std::unordered_set<std::string>& identities);

// Builds structured blocks JSON ({"blocks":[...]}) for people-discovery tool results.
std::string BuildPeopleDiscoveryBlocksJson(const std::vector<DirectoryHit>& directory_hits,
                                           const std::vector<PeopleDiscoveryContactView>& contacts,
                                           const PeopleDiscoveryBuildOptions& options = {});

// If raw text is a directory-hits or contacts JSON array, build blocks JSON; otherwise empty.
std::string TryPeopleDiscoveryBlocksFromToolJson(const std::string& raw_json,
                                                 const PeopleDiscoveryLabels& labels = {});

} // namespace pbr
