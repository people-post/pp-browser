#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** A point in SDL window coordinates (what the drop events carry). */
struct DropPoint {
  float x = 0.f;
  float y = 0.f;
};

/** A point in the UI engine's coordinates (what the shell hit-tests in; see DropPointToUi). */
struct DropUiPoint {
  int x = 0;
  int y = 0;
};

/**
 * SDL window coordinates -> UI coordinates, with the factor the engine's own mouse path multiplies by
 * (SDL_GetWindowPixelDensity; not the display-scale ratio the right-click hook uses), truncated as it truncates.
 */
DropUiPoint DropPointToUi(DropPoint point, float pixel_density);

/**
 * Files dragged onto the window from the OS (DEV/PLAN-desktop-drop-paste.md, C), without any SDL type: SdlAppEvents
 * feeds it the DROP_* events, the clock is passed in. UI thread only.
 *
 * Position. DROP_POSITION is the only place the pointer's place is known. It is kept until the drag ends (COMPLETE),
 * the window loses focus, a DROP_BEGIN says a new drag started, or the next drag's own POSITION replaces it; never by a
 * timer (a pointer held still for 15 s, or a platform that does not repeat positions, must still drop where it was).
 *
 * Hint. The "drop here" hint hides when no POSITION came for more than kHintMs (a still pointer on some platforms sends
 * none): only the hint hides; the drag and its last position stay. A later POSITION shows it again.
 *
 * "Is this position this drag's?" Event order, from the SDL sources (logs/notes/drop-paste-step0): Cocoa and the
 * Windows OLE path send a POSITION right before the FILEs on release, X11 / Wayland have sent positions all along; a
 * drag ends with COMPLETE (a leave, a drop), except a drop macOS rejects, which sends neither FILE nor COMPLETE and
 * so leaves a position behind. A position therefore counts for a FILE batch only when it came after the last
 * boundary (COMPLETE, focus loss, BEGIN, start-up), and a POSITION that arrives while files of an uncompleted drag are
 * still held starts a new drag: those files and that position are dropped. A FILE batch with no position since the
 * last boundary (the Windows WM_DROPFILES fallback, a release SDL sent no position for) yields no position: the
 * caller never takes such a drop. What this cannot tell: a rejected drag's leftover position followed by a drag that
 * brings no position of its own; no platform's source does that (every one that sends positions sends one with the
 * release), and if one ever does, the drop lands where the old drag ended, inside the same window.
 */
class DropCollector {
public:
  static constexpr int64_t kHintMs = 1000;

  /** How a drag ended. */
  struct Completed {
    /** True: it ended with no file (left the window, was cancelled, was text). */
    bool left = false;
    std::vector<std::string> paths;
    /** The last position of this drag; empty = this drag never had one. */
    std::optional<DropPoint> position;
  };

  void OnBegin();
  /** A pointer position over the window. */
  void OnPosition(float x, float y, int64_t now_ms);
  /** One dropped file (the string is copied now: SDL's dies at the next poll). */
  void OnFile(std::string path);
  void OnText() {}
  Completed OnComplete();
  /** The window lost focus: the drag is forgotten. Returns whether the hint was showing. */
  bool OnFocusLost();

  bool HintVisible() const { return hint_visible_; }
  /** The last position of the drag in progress (what the hint is drawn at). */
  std::optional<DropPoint> LastPosition() const { return position_; }
  /** Hides the hint once `kHintMs` have passed since the last POSITION; true = it was hidden by this call. */
  bool Tick(int64_t now_ms);
  /** Time to the next Tick that would hide the hint (the idle wake); empty = nothing pending. */
  std::optional<int64_t> MsUntilHintHide(int64_t now_ms) const;

private:
  void Reset();

  std::optional<DropPoint> position_;
  int64_t position_ms_ = 0;
  bool hint_visible_ = false;
  std::vector<std::string> files_;
};

} // namespace pbr
