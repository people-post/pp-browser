#include "feature/broadcast/BroadcastHub.h"

#include "feature/broadcast/AmpBroadcastRpcClient.h"

#include "domain/mesh/reachability/PeerReachCoordinator.h"
#include "domain/messaging/BroadcastMedia.h"
#include "foundation/runtime/AppRuntime.h"

#include <chrono>
#include <utility>

namespace pbr {

BroadcastHub::BroadcastHub(BroadcastViewerPorts viewer, MediaDeviceArbiter& devices, BroadcasterPorts broadcaster)
    : engine_(std::make_unique<CallMediaEngine>(devices)), capture_engine_(std::make_unique<CallMediaEngine>(devices)) {
  viewer.engine = engine_.get();
  viewer_ = std::make_unique<BroadcastViewerWorkflow>(std::move(viewer));
  broadcaster.engine = capture_engine_.get();
  broadcaster_ = std::make_unique<BroadcasterWorkflow>(std::move(broadcaster));
}

std::unique_ptr<BroadcastHub> BroadcastHub::ForMesh(BroadcastMeshDeps deps, MediaDeviceArbiter& devices) {
  if (!deps.links || !deps.relay.relay || !deps.relay.dial) {
    return nullptr;
  }
  auto rpc = std::make_unique<AmpBroadcastRpcClient>(*deps.links, deps.io.io_pump, deps.io.post_io, deps.io.post_after);
  auto reach = std::make_unique<PeerReachCoordinator>(deps.relay.dial, deps.relay.service_reach);

  BroadcastViewerPorts ports;
  ports.local_peer_id = [peer = deps.io.local_peer_id]() { return peer; };
  ports.publisher_key = std::move(deps.publisher_key);
  ports.reach_peer = [reach = reach.get()](const std::string& peer_id, std::function<void(Roe<void>)> on_done) {
    PeerReachRequest request;
    request.keys = {peer_id};
    request.mode = PeerReachMode::Reach;
    reach->Ensure(std::move(request), [on_done = std::move(on_done)](Roe<PeerReachResult> reached) {
      on_done(reached ? Roe<void>() : Roe<void>(reached.error()));
    });
  };
  ports.request_ticket = [rpc = rpc.get()](const std::string& publisher, const BroadcastTicketRequest& request,
                                           std::function<void(Roe<BroadcastTicketResponse>)> on_done) {
    rpc->RequestTicketAsync(publisher, request, std::move(on_done));
  };
  ports.request_admission = [rpc = rpc.get()](const std::string& hop, const BroadcastViewerAttachRequest& request,
                                              std::function<void(Roe<BroadcastViewerAttachResult>)> on_done) {
    rpc->RequestViewerAttachAsync(hop, request, std::move(on_done));
  };
  ports.relay = deps.relay;
  ports.hop_multiaddr = std::move(deps.hop_multiaddr);
  ports.post_ui = [](std::function<void()> task) { AppRuntime::PostUI(std::move(task)); };
  ports.post_ui_after = [](std::chrono::milliseconds delay, std::function<void()> task) {
    AppRuntime::ScheduleCoordinatorOneShot(delay, [task = std::move(task)]() { AppRuntime::PostUI(task); });
  };
  ports.now_ms = []() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  };

  BroadcasterPorts publish;
  publish.local_peer_id = ports.local_peer_id;
  publish.new_media_key = []() { return NewBroadcastMediaKey(); };
  publish.new_join_handle = [](const std::string& program_id) { return NewBroadcastJoinHandle(program_id); };
  publish.put_program_key = std::move(deps.put_program_key);
  publish.clear_program_key = std::move(deps.clear_program_key);
  publish.announce = std::move(deps.announce);
  publish.relay = ports.relay;
  publish.hop_multiaddr = ports.hop_multiaddr;
  publish.post_ui = ports.post_ui;
  publish.post_ui_after = ports.post_ui_after;

  auto hub = std::make_unique<BroadcastHub>(std::move(ports), devices, std::move(publish));
  hub->rpc_ = std::move(rpc);
  hub->reach_ = std::move(reach);
  return hub;
}

BroadcastHub::~BroadcastHub() {
  broadcaster_.reset();  // ends the show (Ended tip, key cleared) before its engine goes
  viewer_.reset();       // stops the watch (and the engine session) before the engine goes
  if (reach_) {
    reach_->CancelAll();
  }
  if (rpc_) {
    rpc_->Stop();
  }
}

Roe<void> BroadcastHub::WatchLive(const PeerAnnounceTip& tip) {
  auto target = BroadcastWatchTargetFromTip(tip);
  if (!target) {
    return target.error();
  }
  return viewer_->Watch(std::move(*target));
}

void BroadcastHub::StopWatching() {
  viewer_->Stop();
}

bool BroadcastHub::IsWatching() const {
  const auto phase = viewer_->CurrentStatus().phase;
  return phase != BroadcastViewerWorkflow::Phase::Idle && phase != BroadcastViewerWorkflow::Phase::Failed;
}

Roe<void> BroadcastHub::GoLive(BroadcastLiveRequest request) {
  return broadcaster_->GoLive(std::move(request));
}

void BroadcastHub::EndLive() {
  broadcaster_->End();
}

bool BroadcastHub::IsLive() const {
  const auto phase = broadcaster_->CurrentStatus().phase;
  return phase != BroadcasterWorkflow::Phase::Idle && phase != BroadcasterWorkflow::Phase::Failed;
}

void BroadcastHub::SetOnChanged(std::function<void()> callback) {
  viewer_->SetOnStatusChanged(callback);
  broadcaster_->SetOnStatusChanged(std::move(callback));
}

} // namespace pbr
