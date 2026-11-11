#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * The wait between choosing an attachment and seeing something of it (DEV/PLAN-attachment-bubbles-multi-pick.md step
 * 3): the picture's send choice, or the file / video bubble. Each pick gets a rising serial number when its picker
 * opens; every late result (the iOS copy, a video's poster) carries the serial of the pick that started it and is
 * dropped — its files released — when that pick is no longer the current one. Pure: no GUI, no clock (the caller
 * passes the time).
 */
struct PickState {
  uint64_t serial = 0;
  std::string thread_id;
  /** The overlay is wanted (the platform is copying the pick, or a video's poster is being read). */
  bool showing = false;
  int64_t shown_at_ms = 0;
};

/** The second line ("This is a large file, please wait") after this long. */
inline constexpr int64_t kPickSlowMs = 5000;

class AttachmentPicks {
public:
  /** A picker opens in `thread_id`: a new current pick (any older one is over). Its serial. */
  uint64_t Arm(const std::string& thread_id);
  /** The pick `serial` is being prepared (a copy, a poster): the overlay shows from `now_ms`, if it is current. */
  void Show(uint64_t serial, int64_t now_ms);
  /** `serial` is the current pick. */
  bool IsCurrent(uint64_t serial) const;
  /** The pick `serial` ended (any of the plan's seven ways); nothing when another one is current. */
  void End(uint64_t serial);
  /** Whatever pick is current ends (lock, sign-out, the chat deleted, the overlay's Cancel). */
  void EndCurrent();
  const PickState& State() const { return state_; }
  /**
   * A late result of pick `serial` with the files it brought, while `open_thread` is the chat shown now. The chat the
   * pick was armed in when it is current and still open: the caller sends there, and only there. Otherwise nothing:
   * each file is released once, and a current pick whose chat was left meanwhile ends (nothing is sent in the
   * background, and never to the chat open now).
   */
  std::optional<std::string> TakeResult(uint64_t serial, const std::string& open_thread,
                                        const std::vector<std::string>& files,
                                        const std::function<void(const std::string& path)>& release);
  /**
   * The result of pick `serial` would be dropped because its chat is no longer the open one (`open_thread`): the pick
   * is current but was armed elsewhere. The caller says so when the result brought a file (nothing is sent silently).
   */
  bool LeftChat(uint64_t serial, const std::string& open_thread) const;
  /**
   * The send flow for pick `serial` is about to start: refused when another attachment is preparing or uploading
   * (`busy`) — the pick ends, the caller says why and releases the file. True when it may go on.
   */
  bool Accept(uint64_t serial, bool busy);

private:
  uint64_t next_ = 0;
  PickState state_;
};

/**
 * "Offline" that has held for a while: a link that drops for a moment (a Wi-Fi to cellular handover) must not stop an
 * upload that would carry on; only one that stays down for kOfflineStopMs does. Feed it each observation with the time.
 */
inline constexpr int64_t kOfflineStopMs = 3000;
class OfflineHold {
public:
  /** `offline` seen at `now_ms`: true once it has been offline without a break for kOfflineStopMs. */
  bool Update(bool offline, int64_t now_ms);

private:
  int64_t offline_since_ms_ = -1;
};

enum class PickOverlay { Hidden, OneLine, TwoLines };

/**
 * Whether the centred "Preparing…" overlay shows, and with one or two lines: only for a current pick that is being
 * prepared, in the chat it started in, until its send choice or its bubble is up; the second line once it has taken
 * kPickSlowMs.
 */
PickOverlay PickOverlayOf(const PickState& pick, const std::string& open_thread, bool choice_shown, bool bubble_built,
                          int64_t now_ms);

} // namespace pbr
