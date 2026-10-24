#pragma once

#include "common/Module.h"
#include "mesh/types/IDirectoryClient.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

class ContactsStore;
class DirectoryShadowCache;
struct RelayEnvelope;
struct RelayReceiveOutcome;
struct Thread;

/**
 * Where a thread's peer is reached on the relay (its Brief `relay:` id): from the contacts, the
 * Account → `relay:` routes learned from verified inbound envelopes, the directory cache, and — on a
 * send that knows none of those — a directory lookup. One book per messaging stack: send, the
 * control lane and the receive path share it. Any thread.
 */
class PeerRouteBook : public Module {
public:
  explicit PeerRouteBook(ContactsStore& contacts);

  /** Shadows: cached directory hits; directory: a lookup by account on a send's miss. */
  void SetSources(DirectoryShadowCache* shadows, mesh::IDirectoryClient* directory);
  /** Learn Account → `relay:` (a route only, never a directory answer). */
  void NoteAccountRoute(const std::string& account_id, const std::string& relay_user_id);
  /** Learn the sender's route from an inbound envelope — only one whose signature verified. */
  void RememberFromEnvelope(const RelayEnvelope& envelope, const RelayReceiveOutcome& outcome);

  /** From what is known locally (contacts, learned routes, the directory cache). */
  std::optional<std::string> Resolve(const Thread& thread) const;
  /** As Resolve, then a directory lookup by account (blocking: the send path, off the UI thread). */
  std::optional<std::string> ResolveWithDirectory(const Thread& thread);
  /** As Resolve, for an account (no lookup). */
  std::optional<std::string> ResolveAccount(const std::string& account_id) const;
  /** As ResolveWithDirectory, for an account with no thread of its own here (a group member). */
  std::optional<std::string> ResolveAccountWithDirectory(const std::string& account_id);

private:
  ContactsStore& contacts_;
  DirectoryShadowCache* shadows_ = nullptr;
  mesh::IDirectoryClient* directory_ = nullptr;
  mutable std::mutex mutex_;
  /** Inbound-learned Account ID → Brief `relay:` (non-contact call / message). */
  std::unordered_map<std::string, std::string> account_to_relay_;
};

} // namespace pbr
