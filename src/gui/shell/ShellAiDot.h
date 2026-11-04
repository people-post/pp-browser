#pragma once

#include "gui/shell/AiDotModel.h"
#include "lib/ui/ShellTypes.h"

#include <ui/dom/EventListener.h>

#include <cstdint>
#include <functional>
#include <optional>

namespace ui {
class Context;
class Element;
class ElementDocument;
class Event;
}

namespace pbr {

/**
 * The phone AI dot in #shell-ai-dot-mount (window_shell.rml; DESIGN-pp-ui §8.14): shows and hides it with the scene,
 * places it at its edge, follows a drag, slides it to its side, tucks it, and starts its idle actions. Decisions are
 * AiDotModel's; this class only touches the DOM. The bound fields (ai_dot_shown / ai_dot_idle) live in ShellState.
 * Nothing here runs per frame on its own: Update is called by the frames the app draws anyway, and MsUntilNext asks
 * for one more frame only when a tuck or an idle move is due. UI thread.
 */
class ShellAiDot : public ui::EventListener {
public:
  struct Callbacks {
    /** A tap: open the ask-AI panel. */
    std::function<void()> on_tap;
    /** A drag ended at a new resting place (persist it). */
    std::function<void(const AiDotPlace& place, bool tucked)> on_moved;
    /** A bound field changed. */
    std::function<void()> dirty;
  };

  /** Seeds the dot's random idle actions. */
  explicit ShellAiDot(ShellState& state);

  void SetCallbacks(Callbacks callbacks) { callbacks_ = std::move(callbacks); }

  /** Each frame, before Context::Update: follow the scene and the clocks. */
  void Update(ui::Context* context, const AiDotScene& scene);
  /** Milliseconds until the next tuck or idle change, if any (the app may sleep until then). */
  std::optional<int64_t> MsUntilNext() const;

  void ProcessEvent(ui::Event& event) override;
  /** The dot or the document went away (the window document was unloaded). */
  void OnDetach(ui::Element* element) override;

private:
  void Attach(ui::Element* dot);
  void Detach();
  AiDotPlace Place() const { return {state_.ai_dot_left, state_.ai_dot_y_permille}; }
  AiDotArea Area() const;
  float ToDp(float px) const;
  /** Puts the dot's top-left corner at `at`, sliding there over `seconds` (0: at once). */
  void MoveTo(const AiDotPoint& at, float seconds, const std::optional<AiDotPoint>& from = std::nullopt);
  void SetDocumentCapture(bool enabled);
  /** Copies the model's idle move into the bound field. */
  void SyncIdle();
  void EndPress(bool cancelled);

  ShellState& state_;
  Callbacks callbacks_;
  AiDotModel model_;
  ui::Context* context_ = nullptr;
  ui::Element* dot_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  bool document_capture_ = false;
  /** Set while this class removes its own listeners (RmlUi calls OnDetach for those too). */
  bool removing_ = false;
  bool pressed_ = false;
  /** Where the dot was last put (top-left, dp) and whether it was tucked there; nullopt: not placed yet. */
  std::optional<AiDotPoint> placed_;
  bool placed_tucked_ = false;
  /** A slide is running until then: a geometry change meanwhile slides too instead of jumping. */
  int64_t sliding_until_ms_ = 0;
};

} // namespace pbr
