#include "feature/presenters/AccountSyncState.h"

#include <utility>

namespace pbr {

AccountSyncState::AccountSyncState(std::function<void(bool pending)> persist) : persist_(std::move(persist)) {}

bool AccountSyncState::Begin() {
  if (view_ == AccountSyncView::Syncing) {
    return false; // already running its timeout
  }
  view_ = AccountSyncView::Syncing;
  return true;
}

bool AccountSyncState::OnImported() {
  if (persist_) {
    persist_(true);
  }
  return Begin();
}

bool AccountSyncState::OnProfileOpened(const bool stored_pending) {
  if (!stored_pending || view_ != AccountSyncView::None) {
    return false; // a new account, or one already done / running: nothing to show or arm
  }
  return Begin();
}

void AccountSyncState::OnSyncSucceeded() {
  if (view_ == AccountSyncView::None) {
    return; // a normal sync on an account that was never importing
  }
  view_ = AccountSyncView::None;
  if (persist_) {
    persist_(false);
  }
}

void AccountSyncState::OnSyncFailed() {
  if (view_ == AccountSyncView::Syncing) {
    view_ = AccountSyncView::Delayed; // still pending: the next success clears it
  }
}

void AccountSyncState::OnTimeout() {
  OnSyncFailed();
}

} // namespace pbr
