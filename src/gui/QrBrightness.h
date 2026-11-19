#pragma once

#include <functional>
#include <utility>

namespace pbr {

/**
 * Holds the screen at full brightness while a page that shows one of our QR codes is up, and only then: the page's
 * owner reports each frame whether it is showing (`Want`), and the app tells it when it leaves or comes back
 * (`Refresh`). The brightness is raised when some page wants it and the app is on screen, restored otherwise, so every
 * way out (the page closes, a tab or a notification clears it, the app goes to the background) ends in one place.
 * Writes through `set` only on a change. UI thread.
 */
class QrBrightnessHold {
public:
  enum Source : unsigned { kMyQr = 1, kGroupLink = 2 };

  /** `set(true)` raises, `set(false)` restores; `visible()` is whether the app is on screen. */
  QrBrightnessHold(std::function<void(bool)> set, std::function<bool()> visible)
      : set_(std::move(set)), visible_(std::move(visible)) {}

  void Want(const Source source, const bool want) {
    wanted_ = want ? (wanted_ | source) : (wanted_ & ~static_cast<unsigned>(source));
    Refresh();
  }
  /** Re-reads whether the app is on screen (it went to the background or came back). */
  void Refresh() {
    const bool on = wanted_ != 0 && visible_();
    if (on != raised_) {
      raised_ = on;
      set_(on);
    }
  }
  bool Raised() const { return raised_; }

private:
  std::function<void(bool)> set_;
  std::function<bool()> visible_;
  unsigned wanted_ = 0;
  bool raised_ = false;
};

/** The app's one hold: the platform's brightness hook, the app's visibility, re-read on the lifecycle edges. */
QrBrightnessHold& QrBrightness();

} // namespace pbr
