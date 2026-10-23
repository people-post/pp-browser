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

InCallStatus DecideInCallStatus(const InCallSignals& s) {
  using Status = CallScreen::Status;
  using MediaState = CallMediaSeat::MediaState;
  InCallStatus out;
  out.show_retry = s.p2p_connect_failed && !s.awaiting_sfu_recovery && !s.sfu_mode;
  // V037: Connected when MediaChromeLive (InCall + DirectLive|HopLive); the seat's Live is a
  // fallback while the status catches up.
  out.media_connected = s.media_chrome_live || s.seat_media == MediaState::Live;
  const bool media_connecting = s.media_status == CallMediaStatus::DirectConnecting ||
                                s.media_status == CallMediaStatus::HopAttaching ||
                                s.media_status == CallMediaStatus::HopWaiting ||
                                s.media_status == CallMediaStatus::Migrating ||
                                s.seat_media == MediaState::Connecting;
  bool activity = !s.activity.empty();
  if (activity && out.media_connected && !s.awaiting_sfu_recovery) {
    out.activity_spent = true;
    activity = false;
  }
  const bool failed = s.seat_media == MediaState::Failed || s.media_status == CallMediaStatus::Failed;
  if (s.was_connected && !out.media_connected && (s.p2p_connect_failed || failed)) {
    // Was live, then lost its path for good: no retry hint, the screen closes by itself (B89).
    out.status = Status::Disconnected;
    out.show_retry = false;
    out.show_invite = false;
  } else if (s.p2p_connect_failed) {
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
  } else if (s.seat_media == MediaState::Idle && !s.media_active && s.media_status == CallMediaStatus::None) {
    out.status = Status::Calling;
  } else if (s.soft_migrate_in_flight || s.sfu_attach_wait_active || media_connecting) {
    // V037: Migrating / HopWaiting own the line; soft_migrate_in_flight is a latch only.
    if (s.media_status == CallMediaStatus::Migrating ||
        (s.media_status != CallMediaStatus::HopWaiting && s.media_status != CallMediaStatus::HopAttaching &&
         s.soft_migrate_in_flight)) {
      out.status = Status::SettingUpGroup;
    } else if (s.media_status == CallMediaStatus::HopWaiting || s.sfu_attach_wait_active) {
      out.status = Status::WaitingForMediaPath;
    } else {
      out.status = Status::Connecting;
    }
  } else if (!s.media_active) {
    out.status = Status::Calling;
  } else if (s.awaiting_sfu_recovery) {
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
