#include "gui/chat/SendFailureFix.h"

#include "foundation/i18n/LocalizationService.h"

#include <algorithm>

namespace pbr {

RefreshOutcome ClassifyRefresh(const ContactRefreshResult& result) {
  using Devices = ContactRefreshResult::Devices;
  using Directory = ContactRefreshResult::Directory;
  if (result.directory_keys_rejected) {
    return RefreshOutcome::KeysRejected;
  }
  if (!result.NothingAnswered()) {
    return RefreshOutcome::Found;
  }
  switch (result.directory) {
    case Directory::NotFound:
      return RefreshOutcome::NotFound;
    case Directory::Unreachable:
      return RefreshOutcome::Unreachable;
    case Directory::NotAsked:
    case Directory::Answered:
      break;
  }
  return result.devices == Devices::NotReached ? RefreshOutcome::Unreachable : RefreshOutcome::NothingToAsk;
}

SendFixDialog UpdateTheirCardDialog(const bool resendable) {
  return {Tr("chat.send.fix.title"), Tr("chat.send.fix.body"),
          Tr(resendable ? "chat.send.fix.update_and_resend" : "chat.send.fix.update_their_card")};
}

SendFixDialog UpdateMyCardDialog() {
  return {Tr("chat.reconnect.alert_title"), Tr("chat.reconnect.alert_body"), Tr("chat.reconnect.alert_ok")};
}

void SendFailureFix::UpdateTheirCard(const std::string& thread_id, const std::string& message_id,
                                     const std::string& contact_id, const bool resendable) {
  if (!ports_.refresh_contact) {
    return;
  }
  if (ports_.progress) {
    ports_.progress(Tr("chat.send.fix.updating"));
  }
  const uint64_t session = session_;
  const bool started = ports_.refresh_contact(
      contact_id, [this, session, thread_id, message_id, resendable](const ContactRefreshResult& result) {
        if (session != session_) {
          return; // messaging was torn down meanwhile
        }
        std::string text;
        switch (ClassifyRefresh(result)) {
          case RefreshOutcome::Found:
            if (resendable) {
              if (ports_.resend) {
                ports_.resend(thread_id, message_id);
              }
              return;
            }
            text = Tr("chat.send.fix.updated_send_again");
            break;
          case RefreshOutcome::NotFound:
            text = Tr("chat.send.fix.not_found");
            break;
          case RefreshOutcome::Unreachable:
            text = Tr("contacts.refresh.unreachable");
            break;
          case RefreshOutcome::NothingToAsk:
            text = Tr("contacts.refresh.nothing_to_ask");
            break;
          case RefreshOutcome::KeysRejected:
            text = Tr("contacts.refresh.keys_rejected");
            break;
        }
        if (ports_.say) {
          ports_.say(text);
        }
      });
  if (!started && ports_.say) {
    // One refresh of this contact already runs (or the hub is not running): nothing is pending, a later tap starts anew.
    ports_.say(Tr("chat.send.fix.refresh_busy"));
  }
}

void SendFailureFix::UpdateMyCard(const std::string& thread_id, const std::string& message_id, const bool resendable) {
  if (!ports_.update_my_card) {
    return;
  }
  if (updating_) {
    ExpireIfLate(); // an update that never got its release signal does not hold this tap hostage
  }
  const bool known = std::any_of(pending_.begin(), pending_.end(),
                                 [&](const PendingResend& item) { return item.message_id == message_id; });
  if (!known) {
    pending_.push_back({thread_id, message_id, resendable});
  }
  if (updating_) {
    return; // the update in flight (or waiting for its poll) carries this message too: never a second registration
  }
  updating_ = true;
  registered_ = false;
  started_ms_ = NowMs();
  const uint64_t update_id = ++update_id_;
  const uint64_t session = session_;
  ports_.update_my_card([this, session, update_id](const bool ok) {
    if (session != session_ || !updating_ || update_id != update_id_) {
      return;
    }
    if (!ok) {
      DropPending(); // failed, refused, or the PIN was cancelled
      return;
    }
    registered_ = true;
    // The relay answers us again at the next poll that gets through; when none is pending (the recovery never saw
    // the relay refuse us) no such signal will come, and the registration itself is the proof.
    if (!ports_.poll_ok_pending || !ports_.poll_ok_pending()) {
      Release();
    }
  });
}

void SendFailureFix::OnPollOk() {
  if (updating_ && ExpireIfLate()) {
    return; // too late: nothing is sent
  }
  if (updating_ && registered_) {
    Release();
  }
}

void SendFailureFix::Release() {
  std::vector<PendingResend> items = std::move(pending_);
  pending_.clear();
  updating_ = false;
  registered_ = false;
  bool say_again = false;
  for (const PendingResend& item : items) {
    if (item.resendable && ports_.resend) {
      ports_.resend(item.thread_id, item.message_id);
    } else if (!item.resendable) {
      say_again = true;
    }
  }
  if (say_again && ports_.say) {
    ports_.say(Tr("chat.send.fix.updated_send_again"));
  }
}

void SendFailureFix::DropPending() {
  pending_.clear();
  updating_ = false;
  registered_ = false;
  ++update_id_;
}

bool SendFailureFix::ExpireIfLate() {
  if (NowMs() - started_ms_ < kReleaseLimitMs) {
    return false;
  }
  const bool say_again = registered_;
  DropPending();
  if (say_again && ports_.say) {
    ports_.say(Tr("chat.send.fix.updated_send_again"));
  }
  return true;
}

void SendFailureFix::Reset() {
  ++session_;
  DropPending();
}

} // namespace pbr
