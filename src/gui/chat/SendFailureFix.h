#pragma once

#include "feature/conversations/ContactRefreshWorkflow.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pbr {

/** What a contact refresh came to, for the one thing a failed send cares about: did it find the person. */
enum class RefreshOutcome {
  /** A device or our directory knew them (something changed, or it was up to date). */
  Found,
  /** Our directory does not know them. */
  NotFound,
  /** Nobody could be asked (offline, or the contact's devices did not answer). */
  Unreachable,
  /** Nothing to ask: the contact has no Account ID, relay id or device. */
  NothingToAsk,
  /** The directory's keys did not verify. */
  KeysRejected,
};
RefreshOutcome ClassifyRefresh(const ContactRefreshResult& result);

/** The confirm dialog a tap on the red mark of an "update a card first" result shows. */
struct SendFixDialog {
  std::string title;
  std::string body;
  std::string ok_label;
};
SendFixDialog UpdateTheirCardDialog(bool resendable);
SendFixDialog UpdateMyCardDialog();

/**
 * What the user chose in an "update a card first" dialog (a tap on a failed message's red mark): update the contact's
 * card, or our own, and send the message again when the update worked and the message may be sent again. Everything
 * it touches is a port, so tests drive it without a ChatController. UI thread.
 *
 * Pending resends of our own card's update live here, in memory only, keyed by message id; they are dropped when the
 * update fails or is refused, and by Reset (messaging torn down). Nothing else ever sends them.
 */
class SendFailureFix {
public:
  struct Ports {
    /** The same refresh as the contact page's (contacts_ports_.refresh_contact); false: one already runs for it. */
    std::function<bool(const std::string& contact_id, std::function<void(const ContactRefreshResult&)> done)>
        refresh_contact;
    /** Sends the message again through the red mark's own resend path (ChatPresenter::ResendAfterCardUpdate). */
    std::function<void(const std::string& thread_id, const std::string& message_id)> resend;
    /** B216's action (SettingsController::ReconnectAccount): `done(true)` once registered, `done(false)` when not. */
    std::function<void(std::function<void(bool ok)> done)> update_my_card;
    /** The registration recovery is not Ok: a poll that gets through is still to come (OnPollOk). */
    std::function<bool()> poll_ok_pending;
    /** The clock (unix ms) the update's time limit is read from; tests set it. */
    std::function<int64_t()> now_ms;
    /** A progress note, and the answer (the bottom pill). */
    std::function<void(const std::string& text)> progress;
    std::function<void(const std::string& text)> say;
  };

  explicit SendFailureFix(Ports ports = {}) : ports_(std::move(ports)) {}
  void SetPorts(Ports ports) { ports_ = std::move(ports); }

  /** "Update their card" chosen for `message_id`; `resendable`: send it again once they are found. */
  void UpdateTheirCard(const std::string& thread_id, const std::string& message_id, const std::string& contact_id,
                       bool resendable);
  /** "Update my card" chosen for `message_id`. A second one while the update runs only adds its message. */
  void UpdateMyCard(const std::string& thread_id, const std::string& message_id, bool resendable);
  /** A poll got through (the B216 point): the pending messages of an update that worked are sent again. */
  void OnPollOk();
  /** Messaging was torn down: nothing pending, and what is still out ends without effect. */
  void Reset();

  /** How long an update of our card may wait for its release signal before its pending resends are dropped. */
  static constexpr int64_t kReleaseLimitMs = 2 * 60 * 1000;

  bool Pending() const { return !pending_.empty(); }

private:
  struct PendingResend {
    std::string thread_id;
    std::string message_id;
    bool resendable = false;
  };
  void Release();
  void DropPending();
  int64_t NowMs() const { return ports_.now_ms ? ports_.now_ms() : 0; }
  /** The update has waited past kReleaseLimitMs: drops it (says "updated, send again" when its registration worked). */
  bool ExpireIfLate();

  Ports ports_;
  std::vector<PendingResend> pending_;
  /** Our card's update was started and has not ended / been released. */
  bool updating_ = false;
  /** The update worked; waiting for the poll that gets through. */
  bool registered_ = false;
  /** When the update in flight started (NowMs). */
  int64_t started_ms_ = 0;
  /** Up at every start and every drop: a late answer of an update that was dropped is ignored. */
  uint64_t update_id_ = 0;
  /** Up at every Reset: a late answer of the session before it is ignored. */
  uint64_t session_ = 0;
};

} // namespace pbr
