#include "gui/shell/ShellAiDot.h"

#include <ui/base/Tween.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>
#include <ui/style/Property.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

namespace pbr {

namespace {

constexpr const char* kDotId = "shell-ai-dot";
/** Slides (DESIGN-pp-ui §7: short, ease-out): to its side after a drag, into and out of its tuck. */
constexpr float kSnapSeconds = 0.25f;
constexpr float kTuckSeconds = 0.3f;
constexpr float kUntuckSeconds = 0.2f;
/** Replaces a slide still running when the dot must jump (a geometry change): ends on the next frame. */
constexpr float kCutSeconds = 0.01f;

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool Near(const AiDotPoint& a, const AiDotPoint& b) {
  return std::fabs(a.x_dp - b.x_dp) < 0.5f && std::fabs(a.y_dp - b.y_dp) < 0.5f;
}

} // namespace

ShellAiDot::ShellAiDot(ShellState& state) : state_(state) {
  model_.Seed(std::random_device{}());
}

void ShellAiDot::OnDetach(ui::Element* element) {
  if (!removing_ && element != nullptr && (element == dot_ || element == document_)) {
    // Elements are being destroyed: forget them without touching them.
    dot_ = nullptr;
    document_ = nullptr;
    document_capture_ = false;
    pressed_ = false;
    placed_.reset();
    model_.Hide();
  }
}

void ShellAiDot::Attach(ui::Element* dot) {
  Detach();
  if (!dot) {
    return;
  }
  dot_ = dot;
  document_ = dot->GetOwnerDocument();
  dot_->AddEventListener(ui::EventId::Mousedown, this);
  dot_->AddEventListener(ui::EventId::Click, this);
  if (document_) {
    // Any touch or key in the window keeps the dot awake (AiDotModel::Touch; it sleeps after kSleepAfterMs without).
    document_->AddEventListener(ui::EventId::Mousedown, this, true);
    document_->AddEventListener(ui::EventId::Keydown, this, true);
  }
}

void ShellAiDot::Detach() {
  removing_ = true; // RemoveEventListener calls OnDetach back
  SetDocumentCapture(false);
  if (dot_) {
    dot_->RemoveEventListener(ui::EventId::Mousedown, this);
    dot_->RemoveEventListener(ui::EventId::Click, this);
  }
  if (document_) {
    document_->RemoveEventListener(ui::EventId::Mousedown, this, true);
    document_->RemoveEventListener(ui::EventId::Keydown, this, true);
  }
  removing_ = false;
  dot_ = nullptr;
  document_ = nullptr;
  pressed_ = false;
  placed_.reset();
  model_.Hide();
}

float ShellAiDot::ToDp(const float px) const {
  const float ratio = context_ ? context_->GetDensityIndependentPixelRatio() : 1.f;
  return ratio > 0.f ? px / ratio : px;
}

AiDotArea ShellAiDot::Area() const {
  AiDotArea area;
  ui::Element* mount = dot_ ? dot_->GetParentNode() : nullptr;
  if (!mount) {
    return area;
  }
  area.width_dp = ToDp(mount->GetClientWidth());
  area.height_dp = ToDp(mount->GetClientHeight());
  area.safe_top_dp = static_cast<float>(state_.safe_area_top_dp);
  area.safe_bottom_dp = static_cast<float>(state_.safe_area_bottom_dp);
  area.size_dp = AiDotModel::SizeDp(state_.ai_dot_size.c_str());
  return area;
}

void ShellAiDot::MoveTo(const AiDotPoint& at, const float seconds, const std::optional<AiDotPoint>& from) {
  if (!dot_) {
    return;
  }
  const int64_t now = SteadyMs();
  const bool sliding = now < sliding_until_ms_;
  if (seconds <= 0.f && !sliding) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1fdp", at.x_dp);
    dot_->SetProperty("left", buffer);
    std::snprintf(buffer, sizeof(buffer), "%.1fdp", at.y_dp);
    dot_->SetProperty("top", buffer);
  } else {
    // A slide runs on left / top (a user animation keeps its end value); a new one replaces it from where it is.
    const float length = seconds > 0.f ? seconds : kCutSeconds;
    const ui::Tween tween(ui::Tween::Cubic, ui::Tween::Out);
    if (from) {
      const ui::Property from_x(from->x_dp, ui::Unit::DP);
      const ui::Property from_y(from->y_dp, ui::Unit::DP);
      dot_->Animate("left", ui::Property(at.x_dp, ui::Unit::DP), length, tween, 1, false, 0.f, &from_x);
      dot_->Animate("top", ui::Property(at.y_dp, ui::Unit::DP), length, tween, 1, false, 0.f, &from_y);
    } else {
      dot_->Animate("left", ui::Property(at.x_dp, ui::Unit::DP), length, tween);
      dot_->Animate("top", ui::Property(at.y_dp, ui::Unit::DP), length, tween);
    }
    sliding_until_ms_ = now + static_cast<int64_t>(length * 1000.f) + 50;
  }
  placed_ = at;
}

void ShellAiDot::SetDocumentCapture(const bool enabled) {
  if (!document_ || document_capture_ == enabled) {
    document_capture_ = enabled && document_;
    return;
  }
  const bool removing = removing_;
  removing_ = true; // RemoveEventListener calls OnDetach back
  if (enabled) {
    document_->AddEventListener(ui::EventId::Mousemove, this, true);
    document_->AddEventListener(ui::EventId::Mouseup, this, true);
  } else {
    document_->RemoveEventListener(ui::EventId::Mousemove, this, true);
    document_->RemoveEventListener(ui::EventId::Mouseup, this, true);
  }
  removing_ = removing;
  document_capture_ = enabled;
}

void ShellAiDot::SyncIdle() {
  const char* name = AiDotModel::IdleName(model_.Idle());
  const bool asleep = model_.Asleep();
  if (state_.ai_dot_idle != name || state_.ai_dot_asleep != asleep) {
    state_.ai_dot_idle = name;
    state_.ai_dot_asleep = asleep;
    if (callbacks_.dirty) {
      callbacks_.dirty();
    }
  }
}

void ShellAiDot::Update(ui::Context* context, const AiDotScene& scene) {
  context_ = context;
  ui::Element* dot = nullptr;
  if (context && context->GetNumDocuments() > 0) {
    dot = context->GetDocument(0)->GetElementById(kDotId);
  }
  if (dot != dot_) {
    Attach(dot);
  }
  const int64_t now = SteadyMs();
  const bool show = dot_ != nullptr && AiDotModel::Shown(scene);
  if (show != model_.ShownNow()) {
    if (show) {
      model_.Show(now, state_.ai_dot_tucked);
      placed_.reset(); // placed afresh at its edge, without a slide
    } else {
      if (pressed_) {
        EndPress(/*cancelled=*/true);
      }
      model_.Hide();
    }
  }
  bool open = false;
  if (show) {
    open = model_.Advance(now, AiDotModel::LookFrom(state_.ai_dot_look.c_str())).open;
    const AiDotArea area = Area();
    if (!pressed_ && area.width_dp > 0.f) {
      const bool tucked = model_.Tucked();
      const AiDotPoint rest = AiDotModel::Rest(Place(), area, tucked);
      if (!placed_ || !Near(*placed_, rest)) {
        // Into or out of the tuck it slides; anything else (first show, a new screen size, a new place from the
        // settings) puts it there at once.
        const bool tuck_changed = placed_ && tucked != placed_tucked_;
        MoveTo(rest, tuck_changed ? (tucked ? kTuckSeconds : kUntuckSeconds) : 0.f);
        placed_tucked_ = tucked;
      }
    }
  }
  if (state_.ai_dot_shown != show) {
    state_.ai_dot_shown = show;
    if (callbacks_.dirty) {
      callbacks_.dirty();
    }
  }
  SyncIdle();
  if (open && callbacks_.on_tap) {
    callbacks_.on_tap(); // a tap's poke or jump out has played
  }
}

std::optional<int64_t> ShellAiDot::MsUntilNext() const {
  return model_.MsUntilNext(SteadyMs());
}

void ShellAiDot::EndPress(const bool cancelled) {
  SetDocumentCapture(false);
  pressed_ = false;
  if (!dot_) {
    return;
  }
  const int64_t now = SteadyMs();
  dot_->RemoveProperty("transform");
  if (cancelled) {
    model_.Cancel(now);
    placed_.reset();
    return;
  }
  const AiDotArea area = Area();
  const AiDotModel::Release release = model_.Up(area, now);
  if (release.moved) {
    state_.ai_dot_left = release.place.left;
    state_.ai_dot_y_permille = release.place.y_permille;
    state_.ai_dot_tucked = release.tucked;
    // From where the finger left it to its side (tucked: half past it), in one slide (the drag's translate is gone in
    // the same frame).
    MoveTo(AiDotModel::Rest(release.place, area, release.tucked), kSnapSeconds, release.dropped);
    placed_tucked_ = release.tucked;
    if (callbacks_.on_moved) {
      callbacks_.on_moved(release.place, release.tucked);
    }
    return;
  }
  if (release.untucked) {
    // Tapped while tucked: it jumps out (Update slides it to its place out of the tuck) and stays out.
    state_.ai_dot_tucked = false;
    if (callbacks_.on_moved) {
      callbacks_.on_moved(Place(), false);
    }
  }
  // A tap's poke or jump is playing: Update opens the panel when it has (AiDotModel::Changes::open).
  SyncIdle();
}

void ShellAiDot::ProcessEvent(ui::Event& event) {
  if (!dot_) {
    return;
  }
  const auto finger = [this, &event]() {
    ui::Element* mount = dot_->GetParentNode();
    const ui::Vector2f origin = mount ? mount->GetAbsoluteOffset(ui::BoxArea::Border) : ui::Vector2f(0.f, 0.f);
    return AiDotPoint{ToDp(static_cast<float>(event.GetParameter<int>("mouse_x", 0)) - origin.x),
                      ToDp(static_cast<float>(event.GetParameter<int>("mouse_y", 0)) - origin.y)};
  };
  if (document_ && event.GetCurrentElement() == document_ &&
      (event.GetId() == ui::EventId::Mousedown || event.GetId() == ui::EventId::Keydown)) {
    if (model_.Touch(SteadyMs())) {
      SyncIdle();
    }
    return;
  }
  switch (event.GetId()) {
  case ui::EventId::Mousedown: {
    // The dot's own press: nothing under it sees it (no row, no swipe, no scroll starts there).
    event.StopPropagation();
    if (!model_.ShownNow() || pressed_) {
      return;
    }
    const AiDotArea area = Area();
    // Where it stands now: a tucked dot stays half out until it is dragged or let go (a tap makes it jump out).
    model_.Press(finger(), AiDotModel::Rest(Place(), area, model_.Tucked()));
    pressed_ = true;
    SyncIdle();
    SetDocumentCapture(true);
    break;
  }
  case ui::EventId::Mousemove:
    if (pressed_) {
      if (const auto offset = model_.Move(finger(), Area(), SteadyMs())) {
        char buffer[80];
        std::snprintf(buffer, sizeof(buffer), "translate(%.1fdp, %.1fdp)", offset->x_dp, offset->y_dp);
        dot_->SetProperty("transform", buffer);
        event.StopPropagation();
      }
    }
    break;
  case ui::EventId::Mouseup:
    if (pressed_) {
      EndPress(/*cancelled=*/false);
    }
    break;
  case ui::EventId::Click:
    // The tap was taken at mouseup (or it was a drag): the click stops here.
    event.StopPropagation();
    break;
  default:
    break;
  }
}

} // namespace pbr
