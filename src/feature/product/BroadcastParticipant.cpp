#include "feature/product/BroadcastParticipant.h"

#include "domain/broadcast/PeerAnnouncePublisher.h"
#include "domain/media/MediaDeviceArbiter.h"
#include "domain/broadcast/AnnounceOverlayReply.h"
#include "domain/broadcast/PeerAnnounceKeyResolve.h"
#include "domain/people/ContactIdentity.h"
#include "domain/people/PeerDisplayLabel.h"
#include "feature/broadcast/AmpBroadcastTransport.h"
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

BroadcastParticipant::BroadcastParticipant(ConversationsHub& hub) : hub_(hub) {}

BroadcastParticipant::~BroadcastParticipant() {
  Reset();
}

void BroadcastParticipant::SetOnChanged(std::function<void()> callback) {
  on_changed_ = std::move(callback);
}

/** A publisher's device key: ours, or the one pinned / learned for its peer id. */
static std::optional<std::vector<uint8_t>> PublisherKey(ConversationsHub& hub, const std::string& peer_id) {
  std::string local_peer_id;
  std::vector<uint8_t> local_pk;
  if (auto local = hub.Identity().Get()) {
    local_peer_id = local->peer_id;
  }
  if (auto pk = hub.Identity().GetDeviceMlDsaPublicKey()) {
    local_pk = *pk;
  }
  return ResolvePeerAnnouncePublisherKey(
      peer_id, local_peer_id, local_pk,
      [&hub](const std::string& kind, const std::string& value) -> std::optional<std::string> {
        auto record = hub.SigningKeys().Get(kind, value);
        if (!record) {
          return std::nullopt;
        }
        return record->signing_public_key_b64;
      });
}

void BroadcastParticipant::OnMeshServicesStarted(ConversationsHub& hub) {
  services_.reset();
  if (mesh::MeshHost* mesh = hub.Mesh(); mesh && mesh->IsRunning()) {
    if (auto chat = mesh->ChatDeps()) {
      BroadcastServicesDeps deps;
      deps.links = &chat->links;
      deps.io = chat->io;
      if (!deps.io.post_worker) {
        deps.io.post_worker = [](std::function<void()> task) { AppRuntime::PostWorkerNormal(std::move(task)); };
      }
      deps.publisher_key = [&hub](const std::string& peer_id) { return PublisherKey(hub, peer_id); };
      if (auto local = hub.Identity().Get()) {
        deps.local_peer_id = local->peer_id;
      }
      deps.device_secret = [&hub]() -> std::optional<std::vector<uint8_t>> {
        if (auto sk = hub.Identity().GetDeviceMlDsaPrivateKey()) {
          return *sk;
        }
        return std::nullopt;
      };
      services_ = std::make_unique<BroadcastServices>(std::move(deps));
    }
  }
  Rebuild(hub);
}

void BroadcastParticipant::OnMeshStopping(const std::function<void()>&) {
  Reset();
  services_.reset();  // removes its protocol handlers while the mesh is still up
}

void BroadcastParticipant::OnShutdown() {
  Reset();
  services_.reset();
}

void BroadcastParticipant::Reset() {
  deferred_.Invalidate();  // announces still queued for UI drop
  broadcast_.reset();      // its workflows end on the media-sessions owner (the destructor waits)
}

void BroadcastParticipant::Rebuild(ConversationsHub& hub) {
  Reset();
  mesh::MeshHost* mesh = hub.Mesh();
  BroadcastServices* services = services_.get();
  if (!mesh || !mesh->IsRunning() || !services) {
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
  deps.publisher_key = [&hub](const std::string& peer_id) { return PublisherKey(hub, peer_id); };
  deps.put_program_key = [services](const std::string& program_id, const std::string& join_handle,
                                    BroadcastProgramKey key) {
    AmpBroadcastTransport::LiveProgramKey live;
    live.publisher_peer_id = key.publisher_peer_id;
    live.media_key_bytes = std::move(key.media_key);
    live.media_epoch = key.media_epoch;
    live.hop_peer_id = key.hop_peer_id;
    services->PutLiveProgramKey(program_id, join_handle, std::move(live));
  };
  deps.clear_program_key = [services](const std::string& program_id, const std::string& join_handle) {
    services->ClearLiveProgramKey(program_id, join_handle);
  };
  deps.announce = [services](const BroadcastTipDraft& tip) -> Roe<void> {
    PeerAnnouncePublisher::Draft draft;
    draft.topic_id = tip.topic_id;
    draft.program_id = tip.program_id;
    draft.state = tip.state;
    draft.join_handle = tip.join_handle;
    draft.hop_peer_id = tip.hop_peer_id;
    draft.l1_hop_peer_ids = tip.l1_hop_peer_ids;
    draft.video_levels = tip.video_levels;
    auto published = services->PublishTip(draft);
    if (!published) {
      return published.error();
    }
    return {};
  };
  // The broadcaster runs on the media-sessions owner; the announce feed is published from UI.
  deps.post_announce = [this](std::function<void()> task) { AppRuntime::PostUI(deferred_.Bind(std::move(task))); };
  broadcast_ = BroadcastHub::ForMesh(std::move(deps), MediaDeviceArbiter::Default());
  if (!broadcast_) {
    BroadcastLog().info << "broadcast viewer unavailable (media_relay not wired)";
    return;
  }
  broadcast_->SetOnChanged([this]() {
    if (on_changed_) {
      on_changed_();
    }
  });
}

namespace {

/** The DM a reply to an announce publisher goes in: with its contact when we know it. */
struct ReplyPeer {
  std::string contact_id;
  std::string account_id;
  std::string title;
};

ReplyPeer FindReplyPeer(ConversationsHub& hub, const std::string& tip_peer_id) {
  ReplyPeer peer{{}, {}, tip_peer_id};
  if (auto found = hub.Contacts().FindByIdentity(tip_peer_id, mesh::ContactIdKind::PeerId); found && *found) {
    const Contact& contact = **found;
    peer.contact_id = contact.id;
    peer.account_id = PrimaryIdOfKind(contact, mesh::ContactIdKind::Account);
    peer.title = FormatContactTitle(contact);
    if (peer.title.empty()) {
      peer.title = tip_peer_id;
    }
    if (auto* messaging = hub.MeshMessagingOrNull()) {
      messaging->RegisterContactDirectEndpoints(contact);
    }
  }
  return peer;
}

Roe<ThreadMessage> SendReplyDm(ConversationsHub& hub, const DirectChatTarget& target, const std::string& contact_id,
                               const std::string& title, const std::string& text) {
  auto* messaging = hub.MeshMessagingOrNull();
  if (!messaging) {
    return Error("Messaging not ready");
  }
  auto thread = hub.Store().FindOrCreateDirectThread(target, contact_id, title);
  if (!thread) {
    return thread.error();
  }
  messaging->WarmPeerForThread(thread->id);
  return messaging->SendUserMessage(thread->id, text);
}

} // namespace

Roe<ThreadMessage> BroadcastParticipant::ReplyToAnnouncePublisher(const std::string& tip_peer_id,
                                                                  const std::string& text) {
  if (tip_peer_id.empty()) {
    return Error("Missing announce publisher peer_id");
  }
  if (text.empty()) {
    return Error("Empty announce reply");
  }
  const ReplyPeer peer = FindReplyPeer(hub_, tip_peer_id);
  auto plan = PlanAnnounceDmReply(tip_peer_id, peer.contact_id, peer.account_id, peer.title);
  if (!plan) {
    return plan.error();
  }
  return SendReplyDm(hub_, plan->target, plan->contact_id, plan->thread_title, text);
}

Roe<ThreadMessage> BroadcastParticipant::ReplyToAnnounceOverlay(const std::string& tip_peer_id,
                                                                const std::string& join_handle,
                                                                const std::string& text,
                                                                const std::string& viewer_msg_id) {
  if (tip_peer_id.empty()) {
    return Error("Missing announce publisher peer_id");
  }
  if (join_handle.empty() || viewer_msg_id.empty() || text.empty()) {
    return Error("Overlay reply requires join_handle, viewer_msg_id, and text");
  }
  const ReplyPeer peer = FindReplyPeer(hub_, tip_peer_id);
  auto plan = PlanAnnounceOverlayReply(tip_peer_id, peer.contact_id, peer.account_id, peer.title, join_handle, text,
                                       viewer_msg_id);
  if (!plan) {
    return plan.error();
  }
  return SendReplyDm(hub_, plan->dm.target, plan->dm.contact_id, plan->dm.thread_title, plan->message_body);
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
  if (!broadcast_ || !services_) {
    if (on_done) {
      on_done(Unavailable());
    }
    return;
  }
  auto tip = services_->LatestTip(peer_id, topic_id, program_id);
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
