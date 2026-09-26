#pragma once

#include "domain/mesh/host/MeshPorts.h"
#include "domain/messaging/BroadcastRpcCodec.h"

#include "common/Error.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Client side of `/pp-browser/rpc/broadcast/1.0.0` (media-client-layers l4c): a viewer asks the
 * publisher for a ticket and a hop for admission; a relay asks its parent for a slot. One request,
 * one response, on a fresh channel. The serving side (publisher tickets, hop admission) is
 * `AmpBroadcastTransport` in conversations until l5 / relay work moves it.
 *
 * Async completions run on the mesh IO thread (or inline for early failures). Stop() makes every
 * later completion report "stopped"; completions never touch the client object itself.
 */
class AmpBroadcastRpcClient {
public:
  using IoPump = std::function<void()>;
  using IoPost = std::function<void(std::function<void()>)>;
  using IoAfter = std::function<void(std::chrono::milliseconds, std::function<void()>)>;

  static constexpr std::chrono::milliseconds kDefaultTimeout{4000};
  /**
   * Admission asks hops that may not serve the protocol at all (plain relays): Amp acks a channel
   * open for any protocol and the request is then dropped, so only the timeout ends it — keep it
   * short. (A prompt rejection needs pp-cpp-amp to refuse unhandled protocols.)
   */
  static constexpr std::chrono::milliseconds kAdmissionTimeout{1500};

  AmpBroadcastRpcClient(IChatPeerLinks& links, IoPump io_pump, IoPost post_io = {}, IoAfter post_after = {});
  ~AmpBroadcastRpcClient();
  AmpBroadcastRpcClient(const AmpBroadcastRpcClient&) = delete;
  AmpBroadcastRpcClient& operator=(const AmpBroadcastRpcClient&) = delete;

  void Stop();

  void RequestTicketAsync(const std::string& peer_key, const BroadcastTicketRequest& req,
                          std::function<void(Roe<BroadcastTicketResponse>)> on_done);
  void RequestViewerAttachAsync(const std::string& peer_key, const BroadcastViewerAttachRequest& req,
                                std::function<void(Roe<BroadcastViewerAttachResult>)> on_done,
                                std::chrono::milliseconds timeout = kAdmissionTimeout);
  void RequestRelaySlotWinAsync(const std::string& peer_key, const BroadcastRelaySlotWinRequest& req,
                                std::function<void(Roe<BroadcastRelaySlotWinResult>)> on_done);

  // Sync wrappers park on the mesh pump — tests only (never on a thread that drives the mesh).
  Roe<BroadcastTicketResponse> RequestTicket(const std::string& peer_key, const BroadcastTicketRequest& req);
  Roe<BroadcastViewerAttachResult> RequestViewerAttach(const std::string& peer_key,
                                                       const BroadcastViewerAttachRequest& req);
  Roe<BroadcastRelaySlotWinResult> RequestRelaySlotWin(const std::string& peer_key,
                                                       const BroadcastRelaySlotWinRequest& req);

  struct State;

private:
  std::shared_ptr<State> state_;
};

} // namespace pbr
