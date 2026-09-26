#include "feature/broadcast/BroadcastHub.h"

#include <utility>

namespace pbr {

BroadcastHub::BroadcastHub(BroadcastViewerPorts ports, MediaDeviceArbiter& devices)
    : engine_(std::make_unique<CallMediaEngine>(devices)) {
  ports.engine = engine_.get();
  viewer_ = std::make_unique<BroadcastViewerWorkflow>(std::move(ports));
}

BroadcastHub::~BroadcastHub() {
  viewer_.reset();  // stops the watch (and the engine session) before the engine goes
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

void BroadcastHub::SetOnChanged(std::function<void()> callback) {
  viewer_->SetOnStatusChanged(std::move(callback));
}

} // namespace pbr
