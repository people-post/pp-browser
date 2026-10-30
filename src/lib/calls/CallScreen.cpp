#include "lib/calls/CallScreen.h"

namespace pbr {

const char* CallScreenKindName(const CallScreen::Kind kind) {
  switch (kind) {
  case CallScreen::Kind::None:
    return "none";
  case CallScreen::Kind::Ringing:
    return "ringing";
  case CallScreen::Kind::Answering:
    return "answering";
  case CallScreen::Kind::InCall:
    return "in_call";
  }
  return "?";
}

const char* CallScreenStatusName(const CallScreen::Status status) {
  switch (status) {
  case CallScreen::Status::Calling:
    return "calling";
  case CallScreen::Status::Connecting:
    return "connecting";
  case CallScreen::Status::SettingUpGroup:
    return "setting_up_group";
  case CallScreen::Status::WaitingForMediaPath:
    return "waiting_for_media_path";
  case CallScreen::Status::Connected:
    return "connected";
  case CallScreen::Status::Reconnecting:
    return "reconnecting";
  case CallScreen::Status::SendingOnly:
    return "sending_only";
  case CallScreen::Status::CouldntConnect:
    return "couldnt_connect";
  case CallScreen::Status::Disconnected:
    return "disconnected";
  case CallScreen::Status::Activity:
    return "activity";
  case CallScreen::Status::Raw:
    return "raw";
  }
  return "?";
}

bool ShowsParticipantCount(const CallScreen& screen) {
  using Status = CallScreen::Status;
  if (!screen.group || screen.members.empty() || screen.status == Status::Activity) {
    return false;
  }
  if (screen.media_connected) {
    return true;
  }
  const bool regrouping = screen.status == Status::SettingUpGroup || screen.status == Status::WaitingForMediaPath ||
                          screen.status == Status::Connecting;
  return screen.was_connected && regrouping;
}

bool EndedWithoutConnecting(const CallScreen& before, const CallScreen& after, const bool remote_ended) {
  using Status = CallScreen::Status;
  if (remote_ended || before.kind != CallScreen::Kind::InCall || before.was_connected || before.call_id.empty()) {
    return false;
  }
  if (after.kind == CallScreen::Kind::InCall && after.call_id == before.call_id) {
    return false;
  }
  return before.status == Status::CouldntConnect || before.status == Status::Connecting ||
         before.status == Status::SettingUpGroup || before.status == Status::WaitingForMediaPath;
}

InCallStatus DecideInCallStatus(const InCallSignals& s) {
  using Status = CallScreen::Status;
  InCallStatus out;
  const bool recovering = s.recovery != CallRecovery::None;
  out.show_retry = s.p2p_connect_failed && !recovering && !s.sfu_mode;
  // Connected when the call's media is live (InCall + DirectLive|HopLive): the call's own state, not
  // the seat's (a reattach keeps the seat bound while nothing flows).
  out.media_connected = s.media_chrome_live;
  const bool media_connecting = s.media_status == CallMediaStatus::DirectConnecting ||
                                s.media_status == CallMediaStatus::HopAttaching ||
                                s.media_status == CallMediaStatus::HopWaiting ||
                                s.media_status == CallMediaStatus::Migrating;
  bool activity = !s.activity.empty();
  if (activity && out.media_connected && !recovering) {
    out.activity_spent = true;
    activity = false;
  }
  const bool failed = s.media_status == CallMediaStatus::Failed;
  // "Couldn't connect" is a latch: the state machine moving the call onto another path (migrating,
  // attaching to a hop, waiting for one) makes it "connecting" again.
  const bool path_moving = s.media_status == CallMediaStatus::Migrating ||
                           s.media_status == CallMediaStatus::HopAttaching ||
                           s.media_status == CallMediaStatus::HopWaiting;
  if (path_moving) {
    out.show_retry = false;
  }
  if (s.was_connected && !out.media_connected && (s.p2p_connect_failed || failed)) {
    // Was live, then lost its path for good: no retry hint, the screen closes by itself (B89).
    out.status = Status::Disconnected;
    out.show_retry = false;
    out.show_invite = false;
  } else if (s.p2p_connect_failed && !path_moving) {
    out.status = Status::CouldntConnect;
    out.hint = s.group_context ? CallScreen::Hint::GroupMedia : CallScreen::Hint::P2pConnect;
    out.show_invite = false;
  } else if (activity) {
    out.status = Status::Activity;
  } else if (s.media_status == CallMediaStatus::Reconnecting) {
    // k4: the call lost its path and waits for a new one; the call timer keeps running.
    out.status = Status::Reconnecting;
    out.show_elapsed = true;
  } else if (s.media_status == CallMediaStatus::DegradedTxOnly) {
    out.status = Status::SendingOnly;
  } else if (out.media_connected) {
    out.status = Status::Connected;
    out.show_elapsed = true;
    out.show_retry = false;
  } else if (failed) {
    out.status = Status::CouldntConnect;
  } else if (!s.media_active && s.media_status == CallMediaStatus::None) {
    out.status = Status::Calling;
  } else if (s.recovery == CallRecovery::Migrating || s.recovery == CallRecovery::WaitingForHop || media_connecting) {
    // V037: Migrating / HopWaiting own the line; a migrate in flight is a latch only.
    if (s.media_status == CallMediaStatus::Migrating ||
        (s.media_status != CallMediaStatus::HopWaiting && s.media_status != CallMediaStatus::HopAttaching &&
         s.recovery == CallRecovery::Migrating)) {
      out.status = Status::SettingUpGroup;
    } else if (s.media_status == CallMediaStatus::HopWaiting || s.recovery == CallRecovery::WaitingForHop) {
      out.status = Status::WaitingForMediaPath;
    } else {
      out.status = Status::Connecting;
    }
  } else if (!s.media_active) {
    out.status = Status::Calling;
  } else if (recovering) {
    out.status = Status::Reconnecting;
  } else {
    const std::string& state = s.connection_state;
    if (state == "connecting" || state.empty() || state == "new" || state == "closed") {
      out.status = Status::Connecting;
    } else if (state == "disconnected") {
      out.status = Status::Reconnecting;
    } else if (state == "failed") {
      out.status = Status::CouldntConnect;
    } else {
      out.status = Status::Raw;
    }
  }
  return out;
}

} // namespace pbr
