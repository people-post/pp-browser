#include "domain/messaging/CallLifecycleTypes.h"

namespace pbr {

const char* CallPhaseName(const CallPhase phase) {
  switch (phase) {
  case CallPhase::Idle:
    return "Idle";
  case CallPhase::Ringing:
    return "Ringing";
  case CallPhase::Accepting:
    return "Accepting";
  case CallPhase::OutboundCalling:
    return "OutboundCalling";
  case CallPhase::JoinedLocal:
    return "JoinedLocal";
  case CallPhase::MediaPending:
    return "MediaPending";
  case CallPhase::MediaConnecting:
    return "MediaConnecting";
  case CallPhase::InCall:
    return "InCall";
  case CallPhase::ConnectFailed:
    return "ConnectFailed";
  }
  return "Unknown";
}

const char* CallMediaStatusName(const CallMediaStatus status) {
  switch (status) {
  case CallMediaStatus::None:
    return "None";
  case CallMediaStatus::Deciding:
    return "Deciding";
  case CallMediaStatus::DirectConnecting:
    return "DirectConnecting";
  case CallMediaStatus::HopWaiting:
    return "HopWaiting";
  case CallMediaStatus::HopAttaching:
    return "HopAttaching";
  case CallMediaStatus::DirectLive:
    return "DirectLive";
  case CallMediaStatus::HopLive:
    return "HopLive";
  case CallMediaStatus::Migrating:
    return "Migrating";
  case CallMediaStatus::DegradedTxOnly:
    return "DegradedTxOnly";
  case CallMediaStatus::Failed:
    return "Failed";
  case CallMediaStatus::Reconnecting:
    return "Reconnecting";
  }
  return "Unknown";
}

const char* CallArmedPlannerName(const CallArmedPlanner planner) {
  switch (planner) {
  case CallArmedPlanner::None:
    return "None";
  case CallArmedPlanner::Lifecycle:
    return "Lifecycle";
  case CallArmedPlanner::Bridge:
    return "Bridge";
  case CallArmedPlanner::Topology:
    return "Topology";
  }
  return "Unknown";
}

const char* CallLifecycleEventName(const CallLifecycleEvent ev) {
  switch (ev) {
  case CallLifecycleEvent::InviteSeen:
    return "InviteSeen";
  case CallLifecycleEvent::InviteCleared:
    return "InviteCleared";
  case CallLifecycleEvent::OutboundStarted:
    return "OutboundStarted";
  case CallLifecycleEvent::AcceptClicked:
    return "AcceptClicked";
  case CallLifecycleEvent::DeclineClicked:
    return "DeclineClicked";
  case CallLifecycleEvent::LeaveClicked:
    return "LeaveClicked";
  case CallLifecycleEvent::RetryClicked:
    return "RetryClicked";
  case CallLifecycleEvent::MediaDeferred:
    return "MediaDeferred";
  case CallLifecycleEvent::MediaKeyReady:
    return "MediaKeyReady";
  case CallLifecycleEvent::DirectConnected:
    return "DirectConnected";
  case CallLifecycleEvent::ConnectFailedEvt:
    return "ConnectFailed";
  case CallLifecycleEvent::PeerReconnected:
    return "PeerReconnected";
  }
  return "Unknown";
}

} // namespace pbr
