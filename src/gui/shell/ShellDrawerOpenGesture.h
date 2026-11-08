#pragma once

#include "lib/ui/ShellGestureAxis.h"

#include <ui/dom/EventListener.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace ui {
class Context;
class Element;
class ElementDocument;
class Event;
}

namespace pbr {

/**
 * Phone AI page (DEV/PLAN-minimal-nav.md B): a rightward drag across the page opens the AI conversation list (the
 * drawer slides in from the left). Listens on the AI page's minimal block; a press inside the composer, the chips or
 * any field does not arm (those take their own drags). It opens once the drag is more than kOpenDistanceDp to the
 * right and more sideways than up or down; there is no drag-to-close (it would fight the rows' swipes). The drawer
 * does not follow the finger. UI thread; built like ShellSwipeBackGesture.
 */
class ShellDrawerOpenGesture : public ui::EventListener {
public:
  static constexpr float kOpenDistanceDp = 60.f;

  /** A drag of (dx, dy) dp from the press: opens the drawer (rightward past the distance, more sideways than not). */
  static bool Opens(const float dx_dp, const float dy_dp) {
    return dx_dp > kOpenDistanceDp && std::fabs(dy_dp) < dx_dp;
  }

  struct AttachOptions {
    /** A press under an element with one of these ids does not arm. */
    std::vector<std::string> excluded_ids = {"home-composer-mount", "draft-input"};
    /** Nor under one with one of these classes. */
    std::vector<std::string> excluded_classes = {"home-suggestions", "prompt-composer"};
  };

  /** `can_open` is asked at the press and again before opening (the page may have changed under the finger). */
  void Attach(ui::Element* surface, ui::Context* context, AttachOptions options, std::function<bool()> can_open,
              std::function<void()> on_open);
  void Detach();
  bool Attached() const { return surface_ != nullptr; }

  void ProcessEvent(ui::Event& event) override;

private:
  bool Excluded(ui::Element* target) const;
  void SetDocumentCapture(bool enabled);
  void Stop();
  float PxToDp(int px) const;

  ui::Element* surface_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ui::Context* context_ = nullptr;
  AttachOptions options_;
  std::function<bool()> can_open_;
  std::function<void()> on_open_;
  ShellGestureAxisLock axis_lock_;
  bool tracking_ = false;
  bool capture_ = false;
  /** The drag opened the drawer: the click its release produces is dropped (capture stays until that release). */
  bool swallow_click_ = false;
  bool released_ = false;
  /** When that release came; a Click later than a moment after it is not part of the sequence. */
  std::chrono::steady_clock::time_point swallow_click_release_{};
  int start_x_px_ = 0;
  int start_y_px_ = 0;
};

} // namespace pbr
