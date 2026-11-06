#include "gui/chat/GroupJoinPrompt.h"

#include "feature/presenters/GroupLinkText.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/GroupInviteLink.h"

namespace pbr {

namespace {

/** How often a join in flight is looked at (its answer comes on a worker). */
constexpr auto kLookEvery = std::chrono::milliseconds(250);

} // namespace

bool GroupJoinPrompt::Offer(const std::string& text) {
  auto link = group_invite_link::Find(text);
  if (!link) {
    return false;
  }
  if (!ports_.ask) {
    return true;
  }
  const std::string maker = link->maker;
  if (ports_.toast) {
    ports_.toast(Tr("group_join.asking", {{"name", ports_.name_of ? ports_.name_of(maker) : maker}}));
  }
  request_id_.clear();
  ports_.ask(text, [this, maker](Roe<std::string> request_id) { Asked(maker, std::move(request_id)); });
  return true;
}

void GroupJoinPrompt::Asked(const std::string& maker, Roe<std::string> request_id) {
  if (!request_id) {
    if (ports_.toast) {
      ports_.toast(GroupJoinAskFailureText(request_id.error()));
    }
    return;
  }
  request_id_ = std::move(*request_id);
  maker_ = maker;
}

void GroupJoinPrompt::Tick(const std::chrono::steady_clock::time_point now) {
  if (request_id_.empty() || !ports_.state || now - last_look_ < kLookEvery) {
    return;
  }
  last_look_ = now;
  const std::optional<GroupJoinState> state = ports_.state(request_id_);
  if (!state) {
    request_id_.clear();
    return;
  }
  const GroupJoinStep step = NextGroupJoinStep(*state, ports_.name_of ? ports_.name_of(maker_) : maker_);
  if (step.kind == GroupJoinStep::Kind::Wait) {
    return;
  }
  const std::string request_id = request_id_;
  request_id_.clear(); // answered: one sheet or one line per join
  if (step.kind == GroupJoinStep::Kind::Tell) {
    if (ports_.toast) {
      ports_.toast(step.text);
    }
    return;
  }
  if (!ports_.show_confirm) {
    return;
  }
  ports_.show_confirm(step.title, step.text, Tr("group_join.join"), [this, request_id](const bool join) {
    if (!join) {
      if (ports_.cancel) {
        ports_.cancel(request_id);
      }
      return;
    }
    if (!ports_.confirm) {
      return;
    }
    ports_.confirm(request_id, [this](Roe<Thread> thread) {
      if (!thread) {
        if (ports_.toast) {
          ports_.toast(Tr("group_join.err.not_joined"));
        }
        return;
      }
      if (ports_.open_thread) {
        ports_.open_thread(thread->id);
      }
    });
  });
}

} // namespace pbr
