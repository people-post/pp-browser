#pragma once

#include "common/thread/ThreadTypes.h"
#include "lib/calls/CallScreen.h"
#include "lib/calls/CallTypes.h"
#include "lib/people/ContactTypes.h"
#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

class CallUiBackend;

/**
 * Call functional ports for CallController.
 * Application fills from owned CallUiBackend + conversations lookups.
 * Clear via BindCallPorts({}).
 */
struct CallFunctionalPorts {
  std::function<bool()> initialized;
  std::function<CallUiBackend*()> backend;
  std::function<Roe<std::optional<Thread>>(const std::string& thread_id)> get_thread;
  std::function<Roe<std::optional<Contact>>(const std::string& identity, mesh::ContactIdKind kind)>
      find_contact_by_identity;
  std::function<std::string(const std::string& identity)> identity_icon_local_path;
  std::function<std::optional<std::string>()> local_account_identity;
  /** Profile pref OR `--debug` CLI override (V032 call diagnostics). */
  std::function<bool()> call_diagnostics_enabled;
  /** G022: the invites to a group made in the call `call_id` still waiting here, oldest first (the call's banner). */
  std::function<std::vector<CallGroupInvite>(const std::string& call_id)> group_invites_in_call;
  /** G022: the banner's Join — the ordinary accept of the invite; `done` on the UI thread. */
  std::function<void(const std::string& invite_id, std::function<void(Roe<void>)> done)> join_group_invite;
  /**
   * V062 4: "Add {name} to the group" on a guest of the group's call `call_id`: the group's ordinary invite, made in that
   * call (G022's call context: the guest's device takes it from someone it is in the call with); `done` on the UI
   * thread.
   */
  std::function<void(const std::string& group_id, const std::string& identity, const std::string& call_id,
                     std::function<void(Roe<void>)> done)>
      invite_guest_to_group;
  /**
   * A 1:1 invite "who can call me" dropped: the grey local line in the chat with its caller (one per caller,
   * counted per call id; no unread, no sound). UI thread. Unset: nothing recorded.
   */
  std::function<void(const BlockedCallInvite& blocked)> record_blocked_call;
};

} // namespace pbr
