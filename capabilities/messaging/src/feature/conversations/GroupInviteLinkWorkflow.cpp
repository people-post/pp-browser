#include "feature/conversations/GroupInviteLinkWorkflow.h"

#include "lib/messaging/GroupPermissions.h"
#include "lib/messaging/GroupRosterSync.h"

#include "common/Utilities.h"
#include "common/runtime/AppRuntime.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** Previews we sent and joins we asked, at most (a stranger's flood of requests must not grow memory). */
constexpr size_t kMaxOpenJoins = 256;

constexpr const char kOwnLinkError[] = "group_join.err.own_link";

} // namespace

GroupInviteLinkWorkflow::GroupInviteLinkWorkflow(IGroupRosterStore& roster, IGroupInviteLinkStore* links,
                                                 GroupMembershipWorkflow& groups, MeshDeliveryOrchestrator& mesh,
                                                 IdentityStore& identity)
    : roster_(roster), links_(links), groups_(groups), mesh_(mesh), identity_(identity) {
  redirectLogger("GroupInviteLinkWorkflow");
}

GroupInviteLinkWorkflow::~GroupInviteLinkWorkflow() {
  alive_->store(false);
}

Roe<std::string> GroupInviteLinkWorkflow::LocalAccount() const {
  auto id = identity_.Get();
  if (!id) {
    return id.error();
  }
  if (id->account_id.empty()) {
    return Error("Local account identity unavailable");
  }
  return id->account_id;
}

// ---- The maker ----

Roe<GroupInviteLinkWorkflow::MadeLink> GroupInviteLinkWorkflow::MakeLink(const std::string& group_id,
                                                                         const uint64_t max_uses,
                                                                         const int64_t lifetime_ms) {
  auto local = LocalAccount();
  auto device = mesh_.OwnAccountCards().DevicePeerId();
  if (!local || !device) {
    return !local ? local.error() : device.error();
  }
  const int64_t now_ms = util::NowUnixMs();
  const GroupJoinLink link =
      group_invite_link::Mint(*local, now_ms + (lifetime_ms > 0 ? lifetime_ms : group_invite_link::kDefaultLifetimeMs));
  auto draft = group_roster_sync::DraftLinkAdd(roster_, group_id, *local, *device, link.link_id,
                                               group_invite_link::SecretHash(link.link_id, link.secret),
                                               link.expires_at, max_uses, mesh_.GroupChangeCards(), now_ms);
  if (!draft) {
    return draft.error();
  }
  // Kept before it goes out: a link we could not show again is not worth making.
  if (links_) {
    if (auto kept = links_->Keep({.link_id = link.link_id,
                                  .group_id = group_id,
                                  .secret = link.secret,
                                  .maker = *local,
                                  .expires_at = link.expires_at,
                                  .max_uses = max_uses,
                                  .created_at = now_ms});
        !kept) {
      return kept.error();
    }
  }
  if (auto committed = groups_.CommitOwnChange(std::move(*draft)); !committed) {
    if (links_) {
      (void)links_->Forget(link.link_id);
    }
    return committed.error();
  }
  log().info << "invite link made group=" << group_id << " link=" << link.link_id << " max_uses=" << max_uses;
  return MadeLink{.link_id = link.link_id,
                  .text = group_invite_link::Encode(link),
                  .expires_at = link.expires_at,
                  .max_uses = max_uses};
}

Roe<void> GroupInviteLinkWorkflow::RevokeLink(const std::string& group_id, const std::string& link_id) {
  auto local = LocalAccount();
  auto device = mesh_.OwnAccountCards().DevicePeerId();
  if (!local || !device) {
    return !local ? local.error() : device.error();
  }
  auto draft = group_roster_sync::DraftLinkRevoke(roster_, group_id, *local, *device, link_id, mesh_.GroupChangeCards(),
                                                  util::NowUnixMs());
  if (!draft) {
    return draft.error();
  }
  if (auto committed = groups_.CommitOwnChange(std::move(*draft)); !committed) {
    return committed.error();
  }
  log().info << "invite link revoked group=" << group_id << " link=" << link_id;
  return {};
}

std::vector<GroupLinkRow> GroupInviteLinkWorkflow::LiveLinks(const std::string& group_id) {
  std::vector<GroupLinkRow> rows;
  auto local = LocalAccount();
  const int64_t now_ms = util::NowUnixMs();
  const std::optional<GroupRosterView> view =
      group_roster_sync::CurrentView(roster_, group_id, mesh_.GroupChangeCards(), now_ms);
  if (!local || !view) {
    return rows;
  }
  const GroupRosterViewMember* self = view->Find(*local);
  const bool may_revoke_any = self && GroupAllows(GroupAction::RevokeInviteLink, self->role, view->preset);
  for (const GroupInviteLink& link : view->links) {
    GroupLinkRow row{.link_id = link.link_id,
                     .maker = link.maker,
                     .expires_at = link.expires_at,
                     .max_uses = link.max_uses,
                     .uses = link.uses,
                     .can_revoke = self && (link.maker == *local || may_revoke_any)};
    if (link.maker == *local && links_) {
      if (auto kept = links_->Find(link.link_id); kept && *kept && !(*kept)->secret.empty()) {
        row.text = group_invite_link::Encode(
            {.link_id = link.link_id, .secret = (*kept)->secret, .expires_at = link.expires_at, .maker = link.maker});
      }
    }
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end(), [](const GroupLinkRow& a, const GroupLinkRow& b) {
    return a.expires_at != b.expires_at ? a.expires_at < b.expires_at : a.link_id < b.link_id;
  });
  return rows;
}

std::optional<GroupLinkRow> GroupInviteLinkWorkflow::OwnLiveLink(const std::string& group_id) {
  std::optional<GroupLinkRow> newest;
  for (GroupLinkRow& row : LiveLinks(group_id)) {
    if (!row.text.empty() && (!newest || row.expires_at > newest->expires_at)) {
      newest = std::move(row);
    }
  }
  return newest;
}

// ---- The answerer ----

std::optional<GroupJoinRefusal> GroupInviteLinkWorkflow::Judge(const std::string& link_id, const ByteVector& secret,
                                                               const std::string& joiner, const bool count_rate,
                                                               std::string* group_id,
                                                               std::optional<GroupRosterView>* view) {
  auto local = LocalAccount();
  if (!local) {
    return GroupJoinRefusal::Unknown;
  }
  const int64_t now_ms = util::NowUnixMs();
  const GroupCardLookup cards = mesh_.GroupChangeCards();
  std::optional<KeptInviteLink> kept;
  if (links_) {
    if (auto found = links_->Find(link_id); found && *found) {
      kept = std::move(**found);
    }
  }
  if (auto found = group_link_join::FindLinkGroup(roster_, link_id, cards, now_ms)) {
    *group_id = found->group_id;
    *view = std::move(found->view);
  } else if (kept) {
    *group_id = kept->group_id;
    *view = group_roster_sync::CurrentView(roster_, kept->group_id, cards, now_ms);
  }
  const bool known = view->has_value() || kept.has_value();
  group_link_join::JoinCase join{.view = *view ? &**view : nullptr,
                                 .made_here_expires_at = kept ? std::optional<int64_t>(kept->expires_at) : std::nullopt,
                                 .link_id = link_id,
                                 .secret = secret,
                                 .joiner = joiner,
                                 .local_identity = *local,
                                 .now_ms = now_ms,
                                 .within_rate = !count_rate || !known || limiter_.Allow(link_id, now_ms)};
  return group_link_join::JudgeJoin(join);
}

void GroupInviteLinkWorkflow::Send(const std::string& peer, const GroupJoinMessage& message) {
  auto built = group_link_join::Build(message);
  if (!built) {
    return;
  }
  // An account copy carrying our card: the two have never met (a stranger asks, a stranger is answered).
  if (auto sent = mesh_.SendControl(peer, *built, std::nullopt, false, /*with_card=*/true); !sent) {
    log().warning << "join by link: not sent to " << peer << ": " << sent.error().message;
  }
}

void GroupInviteLinkWorkflow::Refuse(const std::string& joiner, const std::string& request_id,
                                     const GroupJoinRefusal reason) {
  log().info << "join by link refused (" << group_link_join::RefusalToString(reason) << ") for " << joiner;
  Send(joiner, GroupJoinRefused{.request_id = request_id, .reason = reason});
}

void GroupInviteLinkWorkflow::DropStale(const int64_t now_ms) {
  for (auto it = answering_.begin(); it != answering_.end();) {
    it = now_ms - it->second.at > group_link_join::kPreviewLifetimeMs ? answering_.erase(it) : std::next(it);
  }
}

void GroupInviteLinkWorkflow::OnRequest(const GroupJoinRequest& request, const std::string& joiner) {
  std::string group_id;
  std::optional<GroupRosterView> view;
  if (const auto refused = Judge(request.link_id, request.secret, joiner, /*count_rate=*/true, &group_id, &view)) {
    Refuse(joiner, request.request_id, *refused);
    return;
  }
  auto device = mesh_.OwnAccountCards().DevicePeerId();
  auto metadata = roster_.LoadMetadata(group_id);
  if (!device || !metadata || !*metadata) {
    Refuse(joiner, request.request_id, GroupJoinRefusal::Unknown);
    return;
  }
  const int64_t now_ms = util::NowUnixMs();
  bool full = false;
  {
    std::lock_guard lock(mutex_);
    DropStale(now_ms);
    full = answering_.size() >= kMaxOpenJoins;
    if (!full) {
      answering_[request.request_id] = {
          .joiner = joiner, .group_id = group_id, .link_id = request.link_id, .secret = request.secret, .at = now_ms};
    }
  }
  if (full) {
    Refuse(joiner, request.request_id, GroupJoinRefusal::Rate);
    return;
  }
  // The group's shared name and how many are in it: never who (G021 4).
  GroupJoinPreview preview{.request_id = request.request_id,
                           .group_id = group_id,
                           .owner_identity = view->owner_identity,
                           .group_title = view->title.empty() ? (*metadata)->title : view->title,
                           .member_count = view->members.size(),
                           .device = *device};
  log().info << "join by link previewed group=" << group_id << " for " << joiner;
  Send(joiner, preview);
}

void GroupInviteLinkWorkflow::OnConfirm(const GroupJoinConfirm& confirm, const std::string& joiner) {
  auto device = mesh_.OwnAccountCards().DevicePeerId();
  if (!device || confirm.device != *device) {
    return; // another of our devices previewed it: that one admits
  }
  Answering answering;
  {
    std::lock_guard lock(mutex_);
    DropStale(util::NowUnixMs());
    auto found = answering_.find(confirm.request_id);
    if (found == answering_.end() || found->second.joiner != joiner) {
      return; // not a preview of ours to them, or forgotten: they ask again
    }
    answering = std::move(found->second);
    answering_.erase(found);
  }
  // The link may have died, or the group filled, since the preview.
  std::string group_id;
  std::optional<GroupRosterView> view;
  if (const auto refused = Judge(answering.link_id, answering.secret, joiner, /*count_rate=*/false, &group_id, &view)) {
    Refuse(joiner, confirm.request_id, *refused);
    return;
  }
  if (auto admitted = groups_.AdmitByLink(group_id, answering.link_id, joiner, confirm.request_id); !admitted) {
    const std::string& why = admitted.error().message;
    Refuse(joiner, confirm.request_id,
           why == kGroupFullError            ? GroupJoinRefusal::GroupFull
           : why == kGroupInviteRemovedError ? GroupJoinRefusal::Removed
                                             : GroupJoinRefusal::Revoked);
  }
}

// ---- The joiner ----

void GroupInviteLinkWorkflow::SetOnJoinChanged(std::function<void(GroupJoinState state)> on_changed) {
  std::lock_guard lock(mutex_);
  on_changed_ = std::move(on_changed);
}

void GroupInviteLinkWorkflow::Moved(const GroupJoinState& state) {
  std::function<void(GroupJoinState)> hook;
  {
    std::lock_guard lock(mutex_);
    hook = on_changed_;
  }
  if (hook) {
    hook(state);
  }
}

Roe<std::string> GroupInviteLinkWorkflow::AskToJoin(const std::string& link_text) {
  auto link = group_invite_link::Find(link_text);
  if (!link) {
    return link.error();
  }
  auto local = LocalAccount();
  if (!local) {
    return local.error();
  }
  if (link->maker == *local) {
    return Error(kOwnLinkError);
  }
  const int64_t now_ms = util::NowUnixMs();
  GroupJoinState state{.request_id = util::GenerateUuid(), .maker = link->maker};
  const bool expired = link->expires_at <= now_ms;
  if (expired) {
    state.phase = GroupJoinState::Phase::Refused;
    state.reason = GroupJoinRefusal::Expired;
  }
  {
    std::lock_guard lock(mutex_);
    for (auto it = joins_.begin(); it != joins_.end() && joins_.size() >= kMaxOpenJoins;) {
      it = now_ms - it->second.second > group_link_join::kPreviewLifetimeMs ? joins_.erase(it) : std::next(it);
    }
    joins_[state.request_id] = {state, now_ms};
  }
  if (!expired) {
    Send(link->maker,
         GroupJoinRequest{.request_id = state.request_id, .link_id = link->link_id, .secret = link->secret});
    ScheduleNoAnswer(state.request_id);
    log().info << "join by link asked of " << link->maker;
  }
  return state.request_id;
}

void GroupInviteLinkWorkflow::ScheduleNoAnswer(const std::string& request_id) {
  AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(group_link_join::kAnswerTimeoutMs), [this, request_id, alive = alive_]() {
        if (!alive->load()) {
          return;
        }
        AppRuntime::PostWorkerNormal([this, request_id, alive]() {
          if (!alive->load() || AppRuntime::IsShuttingDown()) {
            return;
          }
          std::optional<GroupJoinState> moved;
          {
            std::lock_guard lock(mutex_);
            auto found = joins_.find(request_id);
            if (found != joins_.end() && found->second.first.phase == GroupJoinState::Phase::Asking) {
              found->second.first.phase = GroupJoinState::Phase::NoAnswer;
              moved = found->second.first;
            }
          }
          if (moved) {
            Moved(*moved);
          }
        });
      });
}

void GroupInviteLinkWorkflow::OnPreview(const GroupJoinPreview& preview, const std::string& answerer) {
  std::optional<GroupJoinState> moved;
  {
    std::lock_guard lock(mutex_);
    auto found = joins_.find(preview.request_id);
    // The first answer from the link's maker counts (each of its devices may answer); a late one after "no answer" too.
    if (found == joins_.end() || found->second.first.maker != answerer ||
        (found->second.first.phase != GroupJoinState::Phase::Asking &&
         found->second.first.phase != GroupJoinState::Phase::NoAnswer)) {
      return;
    }
    found->second.first.phase = GroupJoinState::Phase::Preview;
    found->second.first.preview = preview;
    found->second.second = util::NowUnixMs();
    moved = found->second.first;
  }
  Moved(*moved);
}

void GroupInviteLinkWorkflow::OnRefused(const GroupJoinRefused& refused, const std::string& answerer) {
  std::optional<GroupJoinState> moved;
  {
    std::lock_guard lock(mutex_);
    auto found = joins_.find(refused.request_id);
    if (found == joins_.end() || found->second.first.maker != answerer ||
        found->second.first.phase == GroupJoinState::Phase::Refused ||
        found->second.first.phase == GroupJoinState::Phase::Cancelled) {
      return;
    }
    // A refusal after our confirm (the link died meanwhile): the chat we made stays, the bundle never comes.
    found->second.first.phase = GroupJoinState::Phase::Refused;
    found->second.first.reason = refused.reason;
    moved = found->second.first;
  }
  Moved(*moved);
}

Roe<Thread> GroupInviteLinkWorkflow::ConfirmJoin(const std::string& request_id) {
  GroupJoinState state;
  {
    std::lock_guard lock(mutex_);
    auto found = joins_.find(request_id);
    if (found == joins_.end() || found->second.first.phase != GroupJoinState::Phase::Preview ||
        !found->second.first.preview) {
      return Error("group_join.err.no_preview");
    }
    if (util::NowUnixMs() - found->second.second > group_link_join::kPreviewLifetimeMs) {
      found->second.first.phase = GroupJoinState::Phase::NoAnswer;
      return Error("group_join.err.no_preview");
    }
    state = found->second.first;
  }
  const GroupJoinPreview& preview = *state.preview;
  auto thread =
      groups_.JoinByLink(request_id, preview.group_id, preview.group_title, preview.owner_identity, state.maker);
  if (!thread) {
    return thread.error();
  }
  Send(state.maker, GroupJoinConfirm{.request_id = request_id, .device = preview.device});
  state.phase = GroupJoinState::Phase::Joined;
  state.thread_id = thread->id;
  {
    std::lock_guard lock(mutex_);
    if (auto found = joins_.find(request_id); found != joins_.end()) {
      found->second.first = state;
    }
  }
  log().info << "join by link confirmed group=" << preview.group_id;
  Moved(state);
  return thread;
}

void GroupInviteLinkWorkflow::CancelJoin(const std::string& request_id) {
  std::lock_guard lock(mutex_);
  if (auto found = joins_.find(request_id); found != joins_.end()) {
    found->second.first.phase = GroupJoinState::Phase::Cancelled;
  }
}

std::optional<GroupJoinState> GroupInviteLinkWorkflow::JoinState(const std::string& request_id) const {
  std::lock_guard lock(mutex_);
  auto found = joins_.find(request_id);
  if (found == joins_.end()) {
    return std::nullopt;
  }
  return found->second.first;
}

} // namespace pbr
