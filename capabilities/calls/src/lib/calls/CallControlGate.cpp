#include "lib/calls/CallControlGate.h"

namespace pbr {

CallControlSender WhoMaySend(const CallControlType type) {
  switch (type) {
    case CallControlType::CallInvite:
      return CallControlSender::Anyone;
    case CallControlType::CallAnsweredElsewhere:
      return CallControlSender::OwnAccount;
    case CallControlType::CallJoinRequest:
    case CallControlType::CallEnded:
      return CallControlSender::OfferedOrInCall;
    case CallControlType::CallAccept:
    case CallControlType::CallDecline:
    case CallControlType::CallLeave:
    case CallControlType::CallRoster:
    case CallControlType::CallMediaKey:
    case CallControlType::CallSfuAttach:
    case CallControlType::CallSfuAttachFailed:
    case CallControlType::CallHopRefuse:
    case CallControlType::CallVideoRefresh:
    case CallControlType::CallCircuitR1:
    case CallControlType::CallPunchOffer:
    case CallControlType::CallPunchAnswer:
    case CallControlType::CallCapsUpdate:
    case CallControlType::CallConnectGiveUp:
    case CallControlType::CallRemove:
      return CallControlSender::InCall;
    case CallControlType::CallSummary:
      return CallControlSender::Nobody;
  }
  return CallControlSender::Nobody; // an unknown value: drop it
}

} // namespace pbr
