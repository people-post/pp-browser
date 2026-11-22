#pragma once

#include <functional>

namespace pbr {

/** What the Contacts and Chats pages show in place of their empty state while an imported account's data comes over. */
enum class AccountSyncView {
  None,    // not importing, or done: the normal lists and empty states
  Syncing, // a spinner and "Syncing your contacts…"
  Delayed  // the first sync failed or timed out: "Still syncing. It will try again on its own." (no spinner)
};

/**
 * The first account-data sync after "I already have an account" (a linking code or a recovery phrase): the contacts
 * (and, after a phrase restore, the groups) arrive through the account locker a few seconds after the import ends, so
 * until that sync has completed an empty list means "not here yet", not "nothing". A new account is never pending.
 *
 * Pending is stored by the owner (ProfilePreferences::account_sync_pending, device-only) so a relaunch before the
 * sync finished shows the indicator again; the state itself starts over then. Not thread-safe: call on the UI thread.
 */
class AccountSyncState {
public:
  /** `persist(true)` when an import marks the account pending, `persist(false)` when the first sync succeeded. */
  explicit AccountSyncState(std::function<void(bool pending)> persist = nullptr);

  /** An account was just brought over: pending now. Returns true when the caller should arm the timeout. */
  bool OnImported();
  /** The profile is open again: `stored_pending` (the saved flag) shows the indicator once more. True: arm the timeout. */
  bool OnProfileOpened(bool stored_pending);
  /** The first locker sync completed (anything new or nothing): the indicator is gone for good. */
  void OnSyncSucceeded();
  /** A locker sync failed (no network, the provider refused): the spinner gives way to the "still syncing" line. */
  void OnSyncFailed();
  /** The timeout armed by OnImported / OnProfileOpened ran out. */
  void OnTimeout();

  AccountSyncView View() const { return view_; }
  bool Pending() const { return view_ != AccountSyncView::None; }

private:
  bool Begin();

  std::function<void(bool)> persist_;
  AccountSyncView view_ = AccountSyncView::None;
};

} // namespace pbr
