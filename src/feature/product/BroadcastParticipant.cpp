#include "feature/product/BroadcastParticipant.h"

#include "domain/broadcast/PeerAnnouncePublisher.h"
#include "domain/media/MediaDeviceArbiter.h"
#include "feature/conversations/AmpBroadcastTransport.h"
#include "feature/conversations/ConversationsHub.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Logger.h"

#include <utility>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

logging::Logger& BroadcastLog() {
  static logging::Logger logger = logging::getLogger("BroadcastParticipant");
  return logger;
}

Roe<void> Unavailable() {
  return Error("Live broadcasts unavailable (mesh / media relay not ready)");
}

} // namespace

BroadcastParticipant::~BroadcastParticipant() {
  Reset();
}

void BroadcastParticipant::SetOnChanged(std::function<void()> callback) {
  on_changed_ = std::move(callback);
}

void BroadcastParticipant::Reset() {
  deferred_.Invalidate();  // announces still queued for UI drop
  broadcast_.reset();      // its workflows end on the media-sessions owner (the destructor waits)
  hub_ = nullptr;
}

void BroadcastParticipant::Rebuild(ConversationsHub& hub) {
  Reset();
  mesh::MeshHost* mesh = hub.Mesh();
  MeshDeliveryOrchestrator* messaging = hub.MeshMessagingOrNull();
  if (!mesh || !mesh->IsRunning() || !messaging) {
    return;
  }
  auto chat = mesh->ChatDeps();
  if (!chat) {
    return;
  }
  BroadcastMeshDeps deps;
  deps.links = &chat->links;
  deps.io = chat->io;
  deps.relay = hub.MediaRelay().RelayAttachPorts();
  deps.lane = hub.Connectivity().Lane();
  deps.publisher_key = [messaging](const std::string& peer_id) { return messaging->ResolveAnnouncePublisherKey(peer_id); };
  deps.put_program_key = [messaging](const std::string& program_id, const std::string& join_handle,
                                     BroadcastProgramKey key) {
    AmpBroadcastTransport::LiveProgramKey live;
    live.publisher_peer_id = key.publisher_peer_id;
    live.media_key_bytes = std::move(key.media_key);
    live.media_epoch = key.media_epoch;
    live.hop_peer_id = key.hop_peer_id;
    if (auto put = messaging->PutLiveProgramKey(program_id, join_handle, std::move(live)); !put) {
      BroadcastLog().warning << "broadcast ticket key not registered: " << put.error().message;
    }
  };
  deps.clear_program_key = [messaging](const std::string& program_id, const std::string& join_handle) {
    messaging->ClearLiveProgramKey(program_id, join_handle);
  };
  deps.announce = [messaging](const BroadcastTipDraft& tip) -> Roe<void> {
    PeerAnnouncePublisher::Draft draft;
    draft.topic_id = tip.topic_id;
    draft.program_id = tip.program_id;
    draft.state = tip.state;
    draft.join_handle = tip.join_handle;
    draft.hop_peer_id = tip.hop_peer_id;
    draft.l1_hop_peer_ids = tip.l1_hop_peer_ids;
    draft.video_levels = tip.video_levels;
    auto published = messaging->PublishAnnounceTip(draft);
    if (!published) {
      return published.error();
    }
    return {};
  };
  // The broadcaster runs on the media-sessions owner; mesh messaging (the announce feed) is UI's.
  deps.post_announce = [this](std::function<void()> task) { AppRuntime::PostUI(deferred_.Bind(std::move(task))); };
  broadcast_ = BroadcastHub::ForMesh(std::move(deps), MediaDeviceArbiter::Default());
  if (!broadcast_) {
    BroadcastLog().info << "broadcast viewer unavailable (media_relay not wired)";
    return;
  }
  hub_ = &hub;
  broadcast_->SetOnChanged([this]() {
    if (on_changed_) {
      on_changed_();
    }
  });
}

void BroadcastParticipant::WatchLive(const PeerAnnounceTip& tip, std::function<void(Roe<void>)> on_done) {
  if (!broadcast_) {
    if (on_done) {
      on_done(Unavailable());
    }
    return;
  }
  broadcast_->WatchLive(tip, std::move(on_done));
}

void BroadcastParticipant::WatchStoredLive(const std::string& peer_id, const std::string& topic_id,
                                           const std::string& program_id, std::function<void(Roe<void>)> on_done) {
  MeshDeliveryOrchestrator* messaging = hub_ ? hub_->MeshMessagingOrNull() : nullptr;
  if (!broadcast_ || !messaging) {
    if (on_done) {
      on_done(Unavailable());
    }
    return;
  }
  auto tip = messaging->LatestAnnounceTip(peer_id, topic_id, program_id);
  if (!tip) {
    if (on_done) {
      on_done(Error("No stored announce for that program"));
    }
    return;
  }
  WatchLive(*tip, std::move(on_done));
}

void BroadcastParticipant::StopWatching() {
  if (broadcast_) {
    broadcast_->StopWatching();
  }
}

std::optional<BroadcastViewerWorkflow::Status> BroadcastParticipant::WatchStatus() const {
  if (!broadcast_) {
    return std::nullopt;
  }
  return broadcast_->Viewer();
}

void BroadcastParticipant::GoLive(const std::string& topic_id, const std::string& program_id,
                                  std::vector<std::string> hops, std::function<void(Roe<void>)> on_done) {
  if (!broadcast_) {
    if (on_done) {
      on_done(Unavailable());
    }
    return;
  }
  broadcast_->GoLive(BroadcastLiveRequest{topic_id, program_id, std::move(hops)}, std::move(on_done));
}

void BroadcastParticipant::EndLive() {
  if (broadcast_) {
    broadcast_->EndLive();
  }
}

std::optional<BroadcasterWorkflow::Status> BroadcastParticipant::LiveStatus() const {
  if (!broadcast_) {
    return std::nullopt;
  }
  return broadcast_->Live();
}

} // namespace pbr
