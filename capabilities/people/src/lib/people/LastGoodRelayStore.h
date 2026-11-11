#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace pbr {

/**
 * The relays that last carried this device's circuits — what the mesh's circuit reach reported choosing
 * (`MeshConnectivity::SetOnRelayChosen`) — most recent first, at most kMaxRelays PeerIds. Kept across restarts so
 * a device that just started parks first where it worked last (`MeshHopPolicy::last_good_relays`; #577,
 * call-path-resilience K015): the mesh itself remembers nothing past one run.
 *
 * Mesh metadata like the directory cache: plaintext in the profile's data dir (read at mesh start, before any PIN
 * unlock), local only, never sent. It only reorders the park's walk; which relays are candidates is the hop
 * policy's to say. Thread-safe.
 */
class LastGoodRelayStore {
public:
  static constexpr size_t kMaxRelays = 3;

  /** `path`: `{profile data dir}/mesh_last_good_relays.json`; empty keeps it in memory. */
  explicit LastGoodRelayStore(std::string path = {});

  const std::string& Path() const { return path_; }
  /** Read the persisted list, if any (a file of another schema is ignored). Not on the mesh's threads. */
  void Load();
  /** `relay_peer_id` carried a circuit just now: it goes first. True when the list changed (then Save). */
  bool Note(const std::string& relay_peer_id);
  /** Write the list as it is now (atomic replace). Not on the mesh's threads. */
  void Save() const;
  /** Most recent first. */
  std::vector<std::string> Relays() const;

private:
  const std::string path_;
  mutable std::mutex mu_;
  std::vector<std::string> relays_; // mu_
  /** Saves in turn: each writes the list as of its turn, so an older one never lands last. */
  mutable std::mutex save_mu_;
};

} // namespace pbr
