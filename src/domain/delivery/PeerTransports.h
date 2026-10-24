#pragma once

#include "domain/delivery/AmpAccountCardTransport.h"
#include "common/Module.h"
#include "common/chat/IDirectMessageClient.h"
#include "common/chat/RelayEnvelope.h"
#include "common/thread/IThreadStore.h"
#include "foundation/crypto/IPskSessionStore.h"
#include "lib/messaging/PeerSigningKeyStore.h"
#include "lib/people/IdentityStore.h"
#include "mesh/host/DisclosureGatedPeerLinks.h"
#include "mesh/host/MeshPorts.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

class IChatBlobPeerClient;
class IChatBlobPeerService;
class IChatHistoryPeerClient;
struct Contact;

/**
 * The mesh transports a messaging stack sends and receives on, for one Amp peer-links instance
 * ([A020] single entry): direct chat, chat history and chat blobs, behind the address-disclosure gate
 * for people-facing traffic; and the peers' dial endpoints. Attach when the mesh is up, detach before
 * it stops (the transports' destructors unregister from the links).
 *
 * Attach / Detach / the setters on the stack's thread; the transports themselves are safe to use
 * from any thread while attached.
 */
class PeerTransports : public Module {
public:
  using WorkerPost = std::function<void(std::function<void()>)>;
  using PostAfter = std::function<void(std::chrono::milliseconds, std::function<void()>)>;

  PeerTransports(IThreadStore& store, IdentityStore& identity, IPeerSigningKeyResolver& signing_key_resolver,
                 IPskSessionStore& psk_store);
  ~PeerTransports() override;
  PeerTransports(const PeerTransports&) = delete;
  PeerTransports& operator=(const PeerTransports&) = delete;

  /** People-facing transports see the links through this gate (set before Attach). */
  void SetAddressDisclosure(const mesh::AddressDisclosureGate* gate) { address_disclosure_ = gate; }
  /** Direct-chat envelopes from peers, with the PeerId of the link each came over (a worker thread). */
  void SetInboundHandler(std::function<void(RelayEnvelope, std::string)> handler) { inbound_ = std::move(handler); }
  /** An account's known devices, so chat reaches an account over a live link to one of them (set before Attach). */
  void SetDevicesOf(std::function<std::vector<std::string>(const std::string&)> devices_of) {
    devices_of_ = std::move(devices_of);
  }
  void SetProfileDataDir(std::string profile_data_dir);

  /** Build the transports on `links` (idempotent while attached to the same links). False: no links. */
  bool Attach(mesh::IChatPeerLinks* links, std::function<void()> io_pump = {}, WorkerPost worker_post = {},
              WorkerPost post_io = {}, PostAfter post_after = {});
  void Detach();
  bool Attached() const { return links_ != nullptr; }

  mesh::IChatPeerLinks* Links() const { return links_; }
  IDirectMessageClient* DirectChat() const { return direct_chat_.get(); }
  IChatHistoryPeerClient* History() const { return history_.get(); }
  IChatBlobPeerClient* BlobClient() const;
  IChatBlobPeerService* BlobService() const;
  /** `/pp/account-card/1.0.0` (null while detached). */
  AmpAccountCardTransport* AccountCards() const { return account_card_.get(); }

  // --- dial endpoints ------------------------------------------------------------------------

  void RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr);
  /** A best-first candidate list (RankAmpDialMultiaddrs order); undialable ADP hosts are dropped. */
  void RegisterEndpoints(const std::string& peer_key, const std::vector<std::string>& multiaddrs);
  /** Everything a contact says about where it listens, ranked for this device. */
  void RegisterContactEndpoints(const Contact& contact);
  /** Warm the association with a peer (a call prefetch shares this with chat). */
  void WarmByKey(const std::string& peer_key);

private:
  IThreadStore& store_;
  IdentityStore& identity_;
  IPeerSigningKeyResolver& signing_key_resolver_;
  IPskSessionStore& psk_store_;
  const mesh::AddressDisclosureGate* address_disclosure_ = nullptr;
  std::function<void(RelayEnvelope, std::string)> inbound_;
  std::function<std::vector<std::string>(const std::string&)> devices_of_;
  std::unique_ptr<AmpAccountCardTransport> account_card_;
  std::string profile_data_dir_;
  mesh::IChatPeerLinks* links_ = nullptr;
  /** `links_` limited to the audience, for the people-facing transports (outlives them). */
  std::unique_ptr<mesh::DisclosureGatedPeerLinks> people_links_;
  std::unique_ptr<IChatHistoryPeerClient> history_;
  std::unique_ptr<IChatBlobPeerService> blob_;
  std::unique_ptr<IDirectMessageClient> direct_chat_;
};

} // namespace pbr
