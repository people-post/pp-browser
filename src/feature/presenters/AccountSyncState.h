#pragma once

#include <cstdint>
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
 * and groups arrive through the account locker a few seconds after the import ends, so
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
  /**
   * A locker sync can never run in this state (no recovery seed, not registered, signed out): nothing is coming, so
   * the indicator goes and the stored flag is cleared — the pages show their normal empty state.
   */
  void OnSyncImpossible();
  /**
   * The timeout armed by OnImported / OnProfileOpened ran out. `token` is TimerToken() as it was when the timer was
   * armed: a timer from before a profile reset (ResetForNewProfile) changes nothing.
   */
  void OnTimeout(uint64_t token);
  /** Taken by each timer the owner arms (the timeout, a retry); a reset makes every earlier one stale. */
  uint64_t TimerToken() const { return timer_token_; }
  /**
   * The profile was reset (Reset my profile): no account, nothing pending; timers armed before it are stale. The
   * stored flag went with the old preferences, so nothing is written.
   */
  void ResetForNewProfile();

  AccountSyncView View() const { return view_; }
  bool Pending() const { return view_ != AccountSyncView::None; }

private:
  bool Begin();

  std::function<void(bool)> persist_;
  AccountSyncView view_ = AccountSyncView::None;
  uint64_t timer_token_ = 0;
};

} // namespace pbr
