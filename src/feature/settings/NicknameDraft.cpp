#include "feature/settings/NicknameDraft.h"

namespace pbr {

void NicknameDraft::OnHydrated(const std::string& from_identity) {
  committed = from_identity;
  ready = true;
  if (!edited) {
    draft = committed;
  }
}

void NicknameDraft::OnIdentityUnavailable() {
  // Keep last known committed/draft. Never treat "not ready" as empty nickname.
}

void NicknameDraft::OnUserEdit(const std::string& value) {
  if (!ready) {
    return;
  }
  draft = value;
  edited = true;
}

bool NicknameDraft::ShouldCommit() const {
  return ready && edited && draft != committed;
}

void NicknameDraft::OnCommitSuccess(const std::string& saved) {
  committed = saved;
  draft = saved;
  edited = false;
  ready = true;
}

void NicknameDraft::ResetEditsToCommitted() {
  draft = committed;
  edited = false;
}

} // namespace pbr
