#pragma once

#include "common/thread/ThreadTypes.h"
#include "mesh/host/MeshPorts.h"
#include "lib/net/OrgBackendClients.h"
#include "common/chat/IDirectMessageClient.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * `/pp/rpc/chat/1.0.0` over AMP ChannelSession (PeerLinkManager::OpenChannel).
 * Product single-entry when MeshHost Amp is attached ([A020]); legacy path remains for tests/fallback.
 *
 * Prefer SendEnvelopeAsync when MeshPump + PostToIo are available; sync SendEnvelope parks.
 */
class AmpDirectChatTransport : public IDirectMessageClient {
public:
  /** A peer's envelope, with the PeerId of the link it came over (authenticated by the link). */
  using InboundHandler = std::function<void(RelayEnvelope envelope, std::string from_peer_id)>;
  /** The PeerIds of an account's devices this device knows (its kept cards), for a key the links do not resolve. */
  using DevicesOf = std::function<std::vector<std::string>(const std::string& key)>;
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;
  using IoPost = std::function<void(std::function<void()>)>;
  using IoAfter = std::function<void(std::chrono::milliseconds, std::function<void()>)>;

  AmpDirectChatTransport(mesh::IChatPeerLinks& links, IoPump io_pump, WorkerPost post_worker = {},
                         IoPost post_io = {}, IoAfter post_after = {});
  ~AmpDirectChatTransport() override;

  AmpDirectChatTransport(const AmpDirectChatTransport&) = delete;
  AmpDirectChatTransport& operator=(const AmpDirectChatTransport&) = delete;

  void Start();
  void Stop();

  void SetInboundHandler(InboundHandler handler);
  void SetDevicesOf(DevicesOf devices_of);
  /**
   * The link key a send to `key` uses: `key` when the links reach it, else one of its devices — one with a
   * live link first; "" when none.
   */
  std::string ReachableKey(const std::string& key) const;

  bool IsPeerReachable(const std::string& peer_identity_value) const override;
  bool IsPeerConnected(const std::string& peer_key) const override;
  DirectSendRoe SendEnvelope(const std::string& peer_relay_user_id, const RelayEnvelope& envelope) override;

  void SendEnvelopeAsync(const std::string& peer_relay_user_id, const RelayEnvelope& envelope,
                         std::function<void(DirectSendRoe)> on_done) override;

  /** Opening the chat channel failed at the link: our code, the link's chain in `message`. */
  static DirectSendFailure WrapLinkFailure(const mesh::IChatPeerLinks::Failure& link);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  mesh::IChatPeerLinks& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  IoPost post_io_;
  IoAfter post_after_;
  DevicesOf devices_of_;
  bool started_ = false;
};

} // namespace pbr
