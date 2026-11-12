#pragma once

#include "lib/calls/CallTypes.h"
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Who may send a call-control type to this device (V057 4): the inbound edge's gate, one row per type.
 * A control from anyone else is dropped silently (a reply would tell the sender we exist).
 */
enum class CallControlSender {
  /** Anyone the "who can call me" audience lets through (an invite). */
  Anyone,
  /** This account's own other devices (CallAnsweredElsewhere; the handler checks the account). */
  OwnAccount,
  /** Someone in the call, or someone a group call was offered to (V053: join requests, its end). */
  OfferedOrInCall,
  /** The call's initiator or a current participant. */
  InCall,
  /** Nobody: not signaling here (the summary is chat history). */
  Nobody,
};

CallControlSender WhoMaySend(CallControlType type);

} // namespace pbr
