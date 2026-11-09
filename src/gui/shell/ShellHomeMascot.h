#pragma once

#include "gui/shell/HomeMascotModel.h"
#include "gui/shell/MascotLook.h"

#include <ui/dom/EventListener.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ui {
class Context;
class Element;
class Event;
}

namespace pbr {

/**
 * The AI Home's logo (#home-logo in home.rml, the phone's minimal Home; Kenneth 2026-11-09): by default the static app
 * icon in the markup; with Me → AI assistant → Animated home logo on, the AI dot's figure drawn large, in the look
 * of the day (LookForDate). A tap plays the poke and then one random action (HomeMascotModel); between taps nothing
 * animates and nothing asks for a frame. It only animates: it never opens the drawer or the ask panel. DOM only; the
 * decisions are HomeMascotModel's and MascotLook.h's. UI thread.
 */
class ShellHomeMascot : public ui::EventListener {
public:
  ShellHomeMascot() = default;
  ~ShellHomeMascot() override { Detach(); }
  ShellHomeMascot(const ShellHomeMascot&) = delete;
  ShellHomeMascot& operator=(const ShellHomeMascot&) = delete;

  /** Me → AI assistant → Animated home logo. */
  void SetAnimated(bool animated) { animated_ = animated; }
  bool Animated() const { return animated_; }

  /** Whether the Home is on screen with nothing over it and the app in front: only then does it move by itself. */
  void SetActive(bool active) { active_ = active; }

  /** Me -> AI assistant -> Look: `auto` (the day's look) or `a`-`d`; the same pref the floating dot follows. */
  void SetLookPref(const std::string& pref);
  /** Tests: the date to take the look from instead of the clock. */
  void SetDateForTest(int year, int month, int day);

  /** Each frame, before Context::Update. */
  void Update(ui::Context* context);
  /** Milliseconds until the running action ends (the app draws one more frame then); nullopt: nothing runs. */
  std::optional<int64_t> MsUntilNext() const;

  void ProcessEvent(ui::Event& event) override;
  void OnDetach(ui::Element* element) override;

  /** The RML put in #home-logo for `look` (a letter): the figure in a dot box. Public for the markup tests. */
  static std::string MascotRml(char look);
  /** The RML of the static logo, as home.rml has it. */
  static const char* StaticRml();

private:
  void Attach(ui::Element* logo);
  void Detach();
  /** Puts the mascot or the static logo in and the running action's class on. */
  void Sync(bool force_classes);
  char TodayLook();

  bool animated_ = true;
  bool active_ = false;
  std::string look_pref_ = "auto";
  ui::Element* logo_ = nullptr;
  /** What the element holds now: the static icon, or the mascot of this look (0: static). */
  char mounted_look_ = 0;
  std::string applied_class_;
  bool removing_ = false;
  HomeMascotModel model_;
  uint32_t seed_ = 0x9e3779b9u;
  int64_t look_checked_ms_ = -1;
  char look_ = 'b';
  struct Date {
    int year, month, day;
  };
  std::optional<Date> test_date_;
};

} // namespace pbr
