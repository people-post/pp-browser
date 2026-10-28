#pragma once

#include "mesh/types/DirectoryTypes.h"
#include "common/directory/IdentityTypes.h"
#include "common/MeshAlias.h"
#include "common/Value.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

/** Narrow contact view for people-discovery chat blocks (avoids people→messaging coupling). */
struct PeopleDiscoveryContactView {
  std::string id;
  std::string display_name;
  std::string server_nickname;
  std::vector<mesh::ContactId> ids;
};

/**
 * User-visible strings of people-discovery blocks. Defaults are English; layers that may use i18n
 * (see lib/ai/LocalizedLabels.h) fill localized text. `{count}`, `{shown}`, `{name}` are placeholders.
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
  std::string show_more_label = "Show more ({rest} left)";
  std::string show_more_message = "Show the next people";
  std::string found_many = "Found {count} people on the network:";
  std::string results_title = "Search results";
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
  /**
   * Rows shown at first, and how many more each "show more" reveals. A directory list carries every hit
   * and shows its first `visible`; a contacts list is cut here (remainder noted in its footer).
   */
  size_t max_visible_items = 10;
};

/**
 * `type` of the local footer action that shows more rows of a long_list in place:
 * `{"type":"show_more_rows","block":<index in blocks[]>,"step":<rows>}`. No turn, no network.
 */
inline constexpr const char* kShowMoreRowsActionType = "show_more_rows";

/** account_id / relay_user_id / peer_id from local identity (non-empty only). */
std::unordered_set<std::string> SelfIdentityValuesFromLocal(const LocalIdentity& identity);

/** True when hit account/ids intersect `identities`. */
bool DirectoryHitMatchesIdentities(const mesh::DirectoryHit& hit,
                                   const std::unordered_set<std::string>& identities);

/** avatar_letter / avatar_tone of a person row, same as the rows of the builders below. */
void SetPersonAvatarFields(pp::common::Object& item, const std::string& display_name, const std::string& stable_id);

// Builds structured blocks JSON ({"blocks":[...]}) for people-discovery tool results.
std::string BuildPeopleDiscoveryBlocksJson(const std::vector<mesh::DirectoryHit>& directory_hits,
                                           const std::vector<PeopleDiscoveryContactView>& contacts,
                                           const PeopleDiscoveryBuildOptions& options = {});

/**
 * Shows `step` more rows of a long_list block (its `visible` grows, capped at its rows). The "show more"
 * footer then names the new remainder, or is dropped once every row shows. False when nothing was hidden.
 */
bool RevealMoreRows(::pp::common::Object& block, size_t step, const PeopleDiscoveryLabels& labels = {});

/** RevealMoreRows on `blocks[block_index]` of a blocks JSON document: the updated document, or nullopt. */
std::optional<std::string> RevealMoreRowsInBlocksJson(const std::string& blocks_json, size_t block_index,
                                                      size_t step, const PeopleDiscoveryLabels& labels = {});

// If raw text is a directory-hits or contacts JSON array, build blocks JSON; otherwise empty.
std::string TryPeopleDiscoveryBlocksFromToolJson(const std::string& raw_json,
                                                 const PeopleDiscoveryLabels& labels = {});

} // namespace pbr
