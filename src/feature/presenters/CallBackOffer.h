#pragma once

#include "lib/calls/CallScreen.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * "Call {name} back?" (V064): End & accept on a ring over a 1:1 call ended that call. Once the call taken in its
 * place is over and no other call is up, the user is offered the ended one again, once.
 *
 * Not offered for a group's call (it usually goes on without us: its chat's call bar offers Join), nor when the
 * ringing caller was that call's peer (the user is talking to them), nor without the call's chat to call from.
 * Pure; CallPresenter feeds it its accepts and frames.
 */
class CallBackOffer {
public:
  /** The ring `ring` (the screen the answer was given on) was answered: `call_id` is the call taken. */
  void OnAccept(const CallScreen& ring, const std::string& call_id);

  /**
   * Every frame (`live_call_id`: the call this device is in, if any): the call to offer, once the call taken is
   * over and nothing else is up. Anything else showing instead (the ended call still up, another call or ring)
   * drops the offer.
   */
  std::optional<CallScreen::EndingCall> OnFrame(const CallScreen& screen, const std::string& live_call_id);

  bool Pending() const { return pending_.has_value(); }

private:
  std::optional<CallScreen::EndingCall> pending_;
  /** The call answered in its place. */
  std::string taken_call_id_;
};

} // namespace pbr
