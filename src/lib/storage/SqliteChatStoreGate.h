#pragma once

#include "common/Error.h"

#include <optional>
#include <string>

#include "common/PbrCompat.h"

namespace pbr {

/** Why a profile's chat store could not be opened after unlock (the app offers a recovery for these). */
enum class ChatStoreProblem {
  /** profile.db is from a newer build than this one (a downgrade). */
  Newer,
  /** A migration step failed and was rolled back, or the file could not be read. */
  NotUpgraded,
};

struct ChatStoreOpenFailure {
  ChatStoreProblem problem = ChatStoreProblem::NotUpgraded;
  std::string message;
};

/**
 * Nothing opens a profile's chat store until its PIN is entered (D105; AT_REST_ENCRYPTION § Unlock). A
 * SqliteThreadDb holds its profile.db closed until it gets the profile's data key, then opens it and runs the
 * ladder; the stores that share the file (rosters, preferences, calls, cards, …) ask here first and are refused
 * while it is not open. Keyed by the profile.db path, process-wide; a path no SqliteThreadDb manages is not gated
 * (a store on its own file, as in a store's own tests).
 */
namespace chat_store_gate {

/** A SqliteThreadDb manages `profile_db_path` (closed); Release when it goes. */
void Hold(const std::string& profile_db_path);
void Release(const std::string& profile_db_path);
/** Its ladder ran: the stores may open the file. */
void MarkOpen(const std::string& profile_db_path);
/** It could not be opened after unlock: the stores are refused with this failure until it is reset. */
void MarkFailed(const std::string& profile_db_path, ChatStoreOpenFailure failure);
/** For a store about to open `profile_db_path`: ok, or why it may not (locked, or its open failure). */
Roe<void> RequireOpen(const std::string& profile_db_path);
/** The open failure of the chat store on `profile_db_path`, if it has one. */
std::optional<ChatStoreOpenFailure> OpenFailure(const std::string& profile_db_path);

} // namespace chat_store_gate

} // namespace pbr
