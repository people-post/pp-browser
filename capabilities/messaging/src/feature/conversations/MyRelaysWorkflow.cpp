#include "feature/conversations/MyRelaysWorkflow.h"

#include <utility>

namespace pbr {

MyRelaysWorkflow::MyRelaysWorkflow(OurRelaysStore& store, Ports ports) : store_(store), ports_(std::move(ports)) {
  redirectLogger("MyRelays");
}

MyRelaysWorkflow::Entered::Entered(const MyRelaysWorkflow& workflow)
    : open([&workflow]() {
        std::lock_guard lock(workflow.gate_mutex_);
        if (workflow.closed_) {
          return false;
        }
        ++workflow.inside_;
        return true;
      }()),
      workflow_(workflow) {}

MyRelaysWorkflow::Entered::~Entered() {
  if (!open) {
    return;
  }
  std::lock_guard lock(workflow_.gate_mutex_);
  if (--workflow_.inside_ == 0) {
    workflow_.gate_cv_.notify_all();
  }
}

void MyRelaysWorkflow::Close() {
  std::unique_lock lock(gate_mutex_);
  closed_ = true;
  gate_cv_.wait(lock, [this]() { return inside_ == 0; });
}

std::string MyRelaysWorkflow::OriginProblemCode(const RelayOriginProblem problem) {
  switch (problem) {
    case RelayOriginProblem::Empty:
      return "relay_origin_empty";
    case RelayOriginProblem::NotAUrl:
      return "relay_origin_not_a_url";
    case RelayOriginProblem::NotHttps:
      return "relay_origin_not_https";
    case RelayOriginProblem::HasPath:
      return "relay_origin_has_path";
    case RelayOriginProblem::HasQuery:
      return "relay_origin_has_query";
    case RelayOriginProblem::TooLong:
      return "relay_origin_too_long";
    case RelayOriginProblem::None:
      break;
  }
  return "";
}

Roe<void> MyRelaysWorkflow::RequireMainDevice() const {
  if (ports_.main_device && !ports_.main_device()) {
    return Error(RelayListProblemCode(RelayListProblem::NotMainDevice));
  }
  return {};
}

Roe<void> MyRelaysWorkflow::Add(const std::string& typed, const bool allow_http) {
  const Entered entered(*this);
  if (!entered.open) {
    return Error(kClosed);
  }
  std::lock_guard lock(change_mutex_);
  if (auto main = RequireMainDevice(); !main) {
    return main;
  }
  const ParsedRelayOrigin parsed = ParseRelayOrigin(typed, allow_http);
  if (parsed.problem != RelayOriginProblem::None) {
    return Error(OriginProblemCode(parsed.problem));
  }
  auto next = WithRelayAdded(store_.List(), parsed.base_url);
  if (!next) {
    return next.error();
  }
  if (ports_.probe) {
    if (auto answered = ports_.probe(parsed.base_url); !answered) {
      log().info << "relay " << parsed.base_url << " not added: it did not answer (" << answered.error().message << ")";
      return Error(kUnreachable);
    }
  }
  log().info << "relay " << parsed.base_url << " added";
  return Commit(*next, false);
}

Roe<void> MyRelaysWorkflow::Remove(const std::string& origin) {
  const Entered entered(*this);
  if (!entered.open) {
    return Error(kClosed);
  }
  std::lock_guard lock(change_mutex_);
  if (auto main = RequireMainDevice(); !main) {
    return main;
  }
  auto next = WithRelayRemoved(store_.List(), origin);
  if (!next) {
    return next.error();
  }
  if (ports_.leave) {
    // Best effort: a relay that is down keeps our row until it expires, but it is no longer ours.
    if (auto left = ports_.leave(origin); !left) {
      log().warning << "registration on " << origin << " not revoked: " << left.error().message;
    }
  }
  log().info << "relay " << origin << " removed";
  return Commit(*next, false);
}

Roe<void> MyRelaysWorkflow::MakePreferred(const std::string& origin) {
  const Entered entered(*this);
  if (!entered.open) {
    return Error(kClosed);
  }
  std::lock_guard lock(change_mutex_);
  if (auto main = RequireMainDevice(); !main) {
    return main;
  }
  const OurRelaysList list = store_.List();
  auto next = WithRelayPreferred(list, origin);
  if (!next) {
    return next.error();
  }
  if (next->Origins() == list.Origins()) {
    return {}; // already the preferred one
  }
  log().info << "relay " << next->Preferred() << " preferred (was " << list.Preferred() << ")";
  return Commit(*next, true);
}

void MyRelaysWorkflow::FollowCard(const std::vector<std::string>& card_relays) {
  const Entered entered(*this);
  if (!entered.open) {
    return;
  }
  std::lock_guard lock(change_mutex_);
  if (card_relays.empty() || (ports_.main_device && ports_.main_device())) {
    return; // the main device's list is its own; an empty card list (mesh only) is not followed in this step
  }
  const OurRelaysList list = store_.List();
  if (list.Origins() == BoundOurRelays(card_relays)) {
    return;
  }
  OurRelaysList next;
  for (const std::string& origin : card_relays) {
    next.relays.push_back(OurRelayEntry{.origin = origin});
  }
  log().info << "following the main device's relays (" << card_relays.size() << ")";
  (void)Commit(next, list.Preferred() != next.Preferred());
}

Roe<void> MyRelaysWorkflow::Commit(const OurRelaysList& next, const bool preferred_changed) {
  const OurRelaysList before = store_.List();
  if (auto kept = store_.Replace(next); !kept) {
    return kept.error();
  }
  if (ports_.apply && !ports_.apply()) {
    // The services stay on the old list: so does the list (nothing was registered or announced for the new one).
    if (auto undone = store_.Replace(before); !undone) {
      log().warning << "relay list not restored after the services did not follow: " << undone.error().message;
    }
    log().warning << "the relay list change was undone: the services did not follow it";
    if (ports_.changed) {
      ports_.changed();
    }
    return Error(kNotApplied);
  }
  if (preferred_changed) {
    RegisterPreferred(store_.List().Preferred());
  }
  if (ports_.register_others) {
    ports_.register_others();
  }
  if (ports_.announce) {
    ports_.announce();
  }
  if (ports_.changed) {
    ports_.changed();
  }
  return {};
}

void MyRelaysWorkflow::RegisterPreferred(const std::string& preferred) {
  if (!ports_.register_preferred) {
    return;
  }
  // The list stands whatever the relay answers: the registration is retried as any (at the next start, or the page's
  // Register); the page says what it answered.
  if (auto registered = ports_.register_preferred(); !registered) {
    log().warning << "registration on the preferred relay " << preferred << " failed: " << registered.error().message;
    store_.Record(preferred, RelayRegistrationRecord{.state = RelayRegistrationRecord::State::Failed,
                                                     .reason = registered.error().message});
    return;
  }
  if (ports_.preferred_registration) {
    if (auto record = ports_.preferred_registration()) {
      store_.Record(preferred, *record);
    }
  }
}

void MyRelaysWorkflow::NotePolled(const std::string& origin, const bool ok, const int64_t now_ms) {
  const Entered entered(*this);
  if (!entered.open) {
    return;
  }
  bool flipped = false;
  {
    std::lock_guard lock(reach_mutex_);
    auto& [was_ok, at_ms] = reach_[RelayOriginKey(origin)];
    flipped = at_ms == 0 || was_ok != ok;
    was_ok = ok;
    at_ms = now_ms;
  }
  if (flipped && ports_.changed) {
    ports_.changed(); // the page shows a change of state, not every poll
  }
}

OurRelaysStatus MyRelaysWorkflow::Status() const {
  OurRelaysStatus status;
  const Entered entered(*this);
  if (!entered.open) {
    return status;
  }
  status.editable = !ports_.main_device || ports_.main_device();
  const OurRelaysList list = store_.List();
  std::optional<RelayRegistrationRecord> preferred;
  if (!list.relays.empty() && ports_.preferred_registration) {
    preferred = ports_.preferred_registration();
  }
  std::lock_guard lock(reach_mutex_);
  for (size_t i = 0; i < list.relays.size(); ++i) {
    const OurRelayEntry& entry = list.relays[i];
    OurRelayStatus relay{.origin = entry.origin, .preferred = i == 0, .registration = entry.registration};
    if (i == 0 && preferred) {
      relay.registration = *preferred; // the identity's; a failed renewal shows until the identity holds one here
    }
    if (const auto reach = reach_.find(RelayOriginKey(entry.origin)); reach != reach_.end()) {
      relay.reach = reach->second.first ? OurRelayStatus::Reach::Ok : OurRelayStatus::Reach::Failed;
      relay.reach_at_ms = reach->second.second;
    }
    status.relays.push_back(std::move(relay));
  }
  return status;
}

} // namespace pbr
