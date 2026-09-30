#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "domain/people/ContactsStore.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Mesh PeerId ↔ account: what a peer's invite / accept told us (in memory), written back onto its
 * contact when it is one, with the contacts as the fallback. Contacts often lack the PeerId — a
 * non-contact call participant is known through the in-memory map only. Not thread-safe: one owner
 * thread (calls owner).
 */
class PeerAccountBook : public Module {
public:
  explicit PeerAccountBook(ContactsStore& contacts);

  /**
   * `account` announced `peer_id`: remembered, and added to its contact's ids when missing.
   * false when either is empty or `account` is not an account identity.
   */
  bool Learn(const std::string& account, const std::string& peer_id);
  /** The PeerId for `account` — learned first, then its contact; nullopt when unknown. */
  Roe<std::optional<std::string>> PeerIdForAccount(const std::string& account) const;
  /**
   * The account behind `peer_id`: learned first, then the `candidates` (e.g. a call's participants)
   * whose contact carries it, then any contact that does.
   */
  Roe<std::optional<std::string>> AccountForPeerId(const std::string& peer_id,
                                                   const std::vector<std::string>& candidates) const;

private:
  void LearnIntoContact(const std::string& account, const std::string& peer_id);

  ContactsStore& contacts_;
  std::unordered_map<std::string, std::string> account_by_peer_id_;
};

} // namespace pbr
