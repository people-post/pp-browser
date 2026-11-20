#pragma once

#include <chrono>
#include <optional>

namespace pbr {

/** What is known when a notification tap is recorded (ShouldArm). */
struct TapCoverFacts {
  /** A message banner's tap (its chat known) or a message alert's (its chat known once the message syncs); a call
   *  alert's tap is not: its ring is what shows. */
  bool message_tap = false;
  /** The app's screen was up when tapped (a notification centre pulled down over it, a desktop window), and a frame had
   *  been drawn: nothing behind the tap is a page left behind. */
  bool app_was_shown = false;
  /** A ring or a call screen (any mode) is up: a live call always shows its call UI, so today's behaviour stays. */
  bool call_shown = false;
  /** A system dialog is in flight (the Save dialog): never closed around, so today's behaviour stays. */
  bool os_modal_up = false;
};

/** What the tapped chat's opening (ChatController::OpenTappedThread) found. */
struct TapOpenFacts {
  bool call_shown = false;
  /** The tap's cover is armed for this tap (the phone case the cover is for). */
  bool cover_armed = false;
  bool messaging_ready = false;
  /** The tapped chat is now the selected one. */
  bool opened = false;
};

/** What to do after the tapped chat's opening was tried. */
enum class TapOpenEnd {
  /** Nothing more: today's behaviour (a missing chat is simply not opened). */
  Nothing,
  /** The cover goes. */
  Release,
  /** The chat list shows (the chat is gone), then the cover goes. */
  ChatListThenRelease,
};

/** The screen while a tap's cover is armed (Shown). */
struct TapCoverScreen {
  /** The app draws (mobile foreground). The cover is already up before (nothing draws), but the hold's clock does not
   *  run: the tap comes in ahead of the foreground, sometimes by seconds (the phone's own unlock). */
  bool foreground = true;
  /** The PIN gate, or the startup logo (cold start: until the unlock is decided, and while it runs), covers the page
   *  itself; the hold waits, its clock starts again once they are gone (the inbox sync starts after the unlock). */
  bool held_elsewhere = false;
  /** A ring or a call screen came up: the cover goes for good (nothing may hide a call). */
  bool call = false;
};

/**
 * A notification tap brings the app to the front for one chat: the page left behind must not show first. While armed,
 * the shell shows the startup cover in its place until the tapped chat is open (Release), for at most kHold of the
 * cover's own showing; then today's behaviour (the page left behind, the chat once its message syncs). Pure: the
 * shell feeds it the time and the screen.
 */
class NotificationTapCover {
public:
  using Clock = std::chrono::steady_clock;
  /** The relay's alert names no chat: one inbox poll after the foreground finds it (0.9-1.4 s on an iPhone). */
  static constexpr std::chrono::milliseconds kHold{3000};

  /** Whether a tap with `facts` is held behind the cover. */
  static bool ShouldArm(const TapCoverFacts& facts) {
    return facts.message_tap && !facts.app_was_shown && !facts.call_shown && !facts.os_modal_up;
  }

  /**
   * Before opening: the tap's chat cannot be opened yet (messaging not ready) while the cover holds it. Nothing is
   * closed or selected and the cover stays (bounded by kHold); the later route (the inbox sync, the wiring) opens it.
   */
  static bool DeferTapOpen(const TapOpenFacts& facts) {
    return facts.cover_armed && !facts.call_shown && !facts.messaging_ready;
  }

  /**
   * After opening was tried. A call keeps its screen (the cover just goes). Without this tap's cover the old behaviour
   * stands (a missing chat does nothing). With it: opened -> the cover goes; not opened -> the chat is gone or cannot
   * open now that messaging is ready: the chat list, then the cover goes.
   */
  static TapOpenEnd EndOfTapOpen(const TapOpenFacts& facts) {
    if (facts.call_shown) {
      return TapOpenEnd::Release;
    }
    if (!facts.cover_armed) {
      return TapOpenEnd::Nothing;
    }
    return facts.opened ? TapOpenEnd::Release : TapOpenEnd::ChatListThenRelease;
  }

  /** A newer tap replaces an armed one: the hold starts over. */
  void Arm() {
    armed_ = true;
    until_.reset();
  }

  /** The tapped chat (or the chat list) is open, or the tap went. True when it was armed. */
  bool Release() {
    const bool was = armed_;
    armed_ = false;
    until_.reset();
    return was;
  }

  bool Armed() const { return armed_; }

  /** Whether the cover is up for `screen` right now: armed, no call, and nothing else covers the page. No state change
   *  (Evaluate, driven once per frame, owns the clock and the release). */
  bool Visible(const TapCoverScreen& screen) const { return armed_ && !screen.call && !screen.held_elsewhere; }

  /** When the hold ends, once its clock runs; nullopt while it waits (or is not armed). */
  std::optional<Clock::time_point> Until() const { return until_; }

  enum class Step { Shown, Hidden, Released, Expired };

  /**
   * The cover's state for `screen` at `now`: Shown (the cover is up), Hidden (armed, but the PIN gate / logo covers the
   * page instead), Released (a call came up, or never armed), Expired (the hold ran out: released now).
   */
  Step Evaluate(const TapCoverScreen& screen, const Clock::time_point now) {
    if (!armed_) {
      return Step::Released;
    }
    if (screen.call) {
      Release();
      return Step::Released;
    }
    if (screen.held_elsewhere) {
      until_.reset();
      return Step::Hidden;
    }
    if (!screen.foreground) {
      until_.reset();
      return Step::Shown;
    }
    if (!until_) {
      until_ = now + kHold;
    }
    if (now >= *until_) {
      Release();
      return Step::Expired;
    }
    return Step::Shown;
  }

private:
  bool armed_ = false;
  std::optional<Clock::time_point> until_;
};

} // namespace pbr
