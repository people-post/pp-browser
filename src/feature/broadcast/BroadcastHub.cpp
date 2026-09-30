#include "feature/broadcast/BroadcastHub.h"

#include "feature/broadcast/AmpBroadcastRpcClient.h"

#include "domain/mesh/reach/PeerReachCoordinator.h"
#include "domain/messaging/BroadcastMedia.h"
#include "foundation/runtime/AppRuntime.h"

#include <chrono>
#include <utility>

namespace pbr {
namespace {

/** Broadcast shares the calls' owner (thread-ownership T001: one media-sessions thread). */
constexpr OwnerThreadId kOwner = OwnerThreadId::MediaSessions;

/** Deliver `on_done` on UI (inline when the runtime has no owner — unit tests without one). */
std::function<void(Roe<void>)> ReplyOnUi(std::function<void(Roe<void>)> on_done) {
  return [on_done = std::move(on_done)](Roe<void> result) {
    if (!on_done) {
      return;
    }
    if (!AppRuntime::HasOwner(kOwner)) {
      on_done(std::move(result));
      return;
    }
    AppRuntime::PostUI([on_done, result = std::move(result)]() { on_done(result); });
  };
}

/** The product runner's thread: the media-sessions owner (inline when the runtime has none). */
class MediaSessionsExecutor final : public OwnerExecutor {
public:
  void Post(std::function<void()> task) override { AppRuntime::PostToOwnerOrRun(kOwner, std::move(task)); }
  void PostFront(std::function<void()> task) override { AppRuntime::PostToFront(kOwner, std::move(task)); }
  TimerId After(const std::chrono::milliseconds delay, std::function<void()> task) override {
    return AppRuntime::ScheduleOn(kOwner, delay, std::move(task));
  }
  void Cancel(const TimerId id) override {
    if (id != 0) {
      AppRuntime::CancelCoordinatorTimer(id);
    }
  }
  bool IsCurrent() const override { return AppRuntime::CurrentlyOn(kOwner); }
  void RunAndWait(const std::function<void()>& task) override { AppRuntime::RunAndWait(kOwner, task); }
};

OwnerExecutor& MediaSessionsOwner() {
  static MediaSessionsExecutor executor;
  return executor;
}

} // namespace

BroadcastHub::BroadcastHub(BroadcastViewerPorts viewer, MediaDeviceArbiter& devices, BroadcasterPorts broadcaster,
                           OwnerExecutor* executor)
    : executor_(executor ? *executor : MediaSessionsOwner()), tasks_(executor_),
      engine_(std::make_unique<CallMediaEngine>(devices)), capture_engine_(std::make_unique<CallMediaEngine>(devices)) {
  viewer.engine = engine_.get();
  viewer_ = std::make_unique<BroadcastViewerWorkflow>(std::move(viewer));
  viewer_->SetOutbox(MakeOwnerOutbox<ViewerEvent>(tasks_, [this](ViewerEvent& event) {
    if (viewer_) {
      viewer_->Handle(event);
      Publish();
    }
  }));
  broadcaster.engine = capture_engine_.get();
  broadcaster_ = std::make_unique<BroadcasterWorkflow>(std::move(broadcaster));
  broadcaster_->SetOutbox(MakeOwnerOutbox<BroadcasterEvent>(tasks_, [this](BroadcasterEvent& event) {
    if (broadcaster_) {
      broadcaster_->Handle(event);
      Publish();
    }
  }));
  frames_sent_ = broadcaster_->FramesSentCounter();
  viewer_->SetOnStatusChanged([this]() { OnStatusChanged(); });
  broadcaster_->SetOnStatusChanged([this]() { OnStatusChanged(); });
  Publish();
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
  // The announce feed belongs to the product hub's thread: run there, answer the broadcaster.
  publish.announce = [announce = std::move(deps.announce), post = std::move(deps.post_announce)](
                         const BroadcastTipDraft& draft, std::function<void(Roe<void>)> on_done) {
    auto run = [announce, draft, on_done = std::move(on_done)]() {
      on_done(announce ? announce(draft) : Roe<void>(Error("peer announce not wired")));
    };
    if (post) {
      post(std::move(run));
    } else {
      run();
    }
  };
  publish.relay = ports.relay;
  publish.hop_multiaddr = ports.hop_multiaddr;

  auto hub = std::make_unique<BroadcastHub>(std::move(ports), devices, std::move(publish));
  hub->rpc_ = std::move(rpc);
  hub->reach_ = std::move(reach);
  return hub;
}

BroadcastHub::~BroadcastHub() {
  // The workflows end on their owner (Ended tip, key cleared, sessions stopped) before the
  // engines go; the owner's queued steps for this hub run first (FIFO).
  executor_.RunAndWait([this]() {
    tasks_.DropPending();
    broadcaster_.reset();
    viewer_.reset();
    on_changed_ = nullptr;
  });
  if (reach_) {
    reach_->CancelAll();
  }
  if (rpc_) {
    rpc_->Stop();
  }
}

void BroadcastHub::OnOwner(std::function<void()> step) {
  tasks_.Post([this, step = std::move(step)]() {
    step();
    Publish();
  });
}

void BroadcastHub::Publish() {
  auto state = std::make_shared<BroadcastUiState>();
  if (viewer_) {
    state->viewer = viewer_->CurrentStatus();
  }
  if (broadcaster_) {
    state->live = broadcaster_->CurrentStatus();
  }
  std::lock_guard lock(state_mu_);
  state_ = std::move(state);
}

void BroadcastHub::OnStatusChanged() {
  Publish();
  if (!on_changed_) {
    return;
  }
  if (AppRuntime::HasOwner(kOwner)) {
    AppRuntime::PostUI(on_changed_);
  } else {
    on_changed_();
  }
}

std::shared_ptr<const BroadcastUiState> BroadcastHub::State() const {
  std::lock_guard lock(state_mu_);
  return state_;
}

void BroadcastHub::WatchLive(const PeerAnnounceTip& tip, std::function<void(Roe<void>)> on_done) {
  OnOwner([this, tip, reply = ReplyOnUi(std::move(on_done))]() {
    auto target = BroadcastWatchTargetFromTip(tip);
    if (!target) {
      reply(target.error());
      return;
    }
    reply(viewer_->Watch(std::move(*target)));
  });
}

void BroadcastHub::StopWatching() {
  OnOwner([this]() { viewer_->Stop(); });
}

bool BroadcastHub::IsWatching() const {
  const auto phase = State()->viewer.phase;
  return phase != BroadcastViewerWorkflow::Phase::Idle && phase != BroadcastViewerWorkflow::Phase::Failed;
}

void BroadcastHub::GoLive(BroadcastLiveRequest request, std::function<void(Roe<void>)> on_done) {
  OnOwner([this, request = std::move(request), reply = ReplyOnUi(std::move(on_done))]() mutable {
    reply(broadcaster_->GoLive(std::move(request)));
  });
}

void BroadcastHub::EndLive() {
  OnOwner([this]() { broadcaster_->End(); });
}

bool BroadcastHub::IsLive() const {
  const auto phase = State()->live.phase;
  return phase != BroadcasterWorkflow::Phase::Idle && phase != BroadcasterWorkflow::Phase::Failed;
}

BroadcasterWorkflow::Status BroadcastHub::Live() const {
  BroadcasterWorkflow::Status live = State()->live;
  if (frames_sent_) {
    live.frames_sent = frames_sent_->load(std::memory_order_relaxed);
  }
  return live;
}

void BroadcastHub::SetOnChanged(std::function<void()> callback) {
  OnOwner([this, callback = std::move(callback)]() { on_changed_ = callback; });
}

} // namespace pbr
