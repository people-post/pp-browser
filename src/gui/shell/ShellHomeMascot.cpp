#include "gui/shell/ShellHomeMascot.h"

#include "gui/shell/MascotFigure.h"
#include "lib/ui/CalendarHelper.h"

#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>

#include <chrono>
#include <random>

namespace pbr {

namespace {

constexpr const char* kLogoId = "home-logo";
constexpr const char* kMascotClass = "home-logo--mascot";
/** The look is read from the clock at most this often (a midnight passes while the app is open). */
constexpr int64_t kLookRecheckMs = 60000;

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

const char* ShellHomeMascot::StaticRml() {
  return "<img src=\"../branding/app-icon.png\" alt=\"\"/>";
}

std::string ShellHomeMascot::MascotRml(const char look) {
  const char parts = PartsFor(look);
  std::string classes = "ai-dot ai-dot--shown home-mascot";
  if (AiDotModel::Calm(DotLookOf(parts))) {
    classes += " ai-dot--calm";
  }
  if (parts == 'd') {
    classes += " ai-dot--plain";
  }
  return "<div id=\"home-mascot\" class=\"" + classes + "\">" + MascotFigureRml(parts) + "</div>";
}

void ShellHomeMascot::SetLookPref(const std::string& pref) {
  look_pref_ = IsLookPref(pref) ? pref : "auto";
  look_checked_ms_ = -1;
}

void ShellHomeMascot::SetDateForTest(const int year, const int month, const int day) {
  test_date_ = Date{year, month, day};
  look_checked_ms_ = -1;
}

char ShellHomeMascot::TodayLook() {
  const int64_t now = SteadyMs();
  if (look_checked_ms_ < 0 || test_date_ || now - look_checked_ms_ >= kLookRecheckMs) {
    look_checked_ms_ = now;
    Date date{};
    if (test_date_) {
      date = *test_date_;
    } else {
      const TodayYmd today = TodayLocalDate();
      date = {today.year, today.month, today.day};
    }
    look_ = ResolveLook(look_pref_, date.year, date.month, date.day);
  }
  return look_;
}

void ShellHomeMascot::OnDetach(ui::Element* element) {
  if (!removing_ && element != nullptr && element == logo_) {
    // The Home view is being unloaded: forget the element without touching it.
    logo_ = nullptr;
    mounted_look_ = 0;
    applied_class_.clear();
    model_ = HomeMascotModel{};
  }
}

void ShellHomeMascot::Attach(ui::Element* logo) {
  Detach();
  if (!logo) {
    return;
  }
  logo_ = logo;
  logo_->AddEventListener(ui::EventId::Click, this);
}

void ShellHomeMascot::Detach() {
  if (logo_) {
    removing_ = true; // RemoveEventListener calls OnDetach back
    logo_->RemoveEventListener(ui::EventId::Click, this);
    removing_ = false;
  }
  logo_ = nullptr;
  mounted_look_ = 0;
  applied_class_.clear();
  model_ = HomeMascotModel{};
}

void ShellHomeMascot::Sync(const bool force_classes) {
  if (!logo_) {
    return;
  }
  const char want = animated_ ? TodayLook() : static_cast<char>(0);
  if (want != mounted_look_) {
    // The icon in home.rml is what a view starts with; the mascot replaces it, and the icon comes back when off.
    logo_->SetInnerRML(want != 0 ? MascotRml(want).c_str() : StaticRml());
    logo_->SetClass(kMascotClass, want != 0);
    mounted_look_ = want;
    applied_class_.clear();
    model_ = HomeMascotModel{};
  }
  if (mounted_look_ == 0) {
    return;
  }
  const std::string name = AiDotModel::IdleName(model_.Idle());
  if (force_classes || name != applied_class_) {
    if (ui::Element* mascot = logo_->GetFirstChild()) {
      if (!applied_class_.empty()) {
        mascot->SetClass("ai-dot--" + applied_class_, false);
      }
      if (!name.empty()) {
        mascot->SetClass("ai-dot--" + name, true);
      }
    }
    applied_class_ = name;
  }
}

void ShellHomeMascot::Update(ui::Context* context) {
  ui::Element* logo = nullptr;
  if (context && context->GetNumDocuments() > 0) {
    logo = context->GetDocument(0)->GetElementById(kLogoId);
  }
  if (logo != logo_) {
    Attach(logo);
  }
  if (!logo_) {
    return;
  }
  model_.Advance(SteadyMs());
  Sync(false);
}

std::optional<int64_t> ShellHomeMascot::MsUntilNext() const {
  return model_.MsUntilNext(SteadyMs());
}

void ShellHomeMascot::ProcessEvent(ui::Event& event) {
  if (event.GetId() != ui::EventId::Click || !logo_) {
    return;
  }
  // A tap on the logo only animates it: nothing under it (the drawer's edge swipe, the field) sees it.
  event.StopPropagation();
  if (mounted_look_ == 0) {
    return;
  }
  seed_ = seed_ * 1664525u + 1013904223u + static_cast<uint32_t>(SteadyMs());
  model_.Tap(SteadyMs(), DotLookOf(PartsFor(mounted_look_)), seed_ >> 8);
  Sync(false);
}

} // namespace pbr
