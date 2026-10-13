#pragma once

#include "gui/onboarding/OnboardingPorts.h"
#include "gui/onboarding/OnboardingRules.h"

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Event.h>
#include <ui/base/Types.h>

#include <string>

namespace pbr {

/**
 * First-run wizard after the PIN gate: name + set up account → your account id → find friends.
 * Presentation only; everything it does goes through OnboardingPorts.
 */
class OnboardingController {
public:
  OnboardingController() = default;
  static OnboardingController& Instance();
  static void InstallInstance(OnboardingController& controller);
  static void ClearInstance();

  void BindPorts(OnboardingPorts ports);
  bool RegisterModel(ui::Context* context);

  /** After unlock: show the wizard when the rules say so (see OnboardingStartStep). */
  void MaybeStart();
  /** The wizard will not show again (finished, skipped, or an account was brought over). */
  void MarkCompleted();
  bool IsActive() const { return active_; }

private:
  static void RegisterCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void CopyIdCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ShareCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void NextCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void FindFriendsCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void FinishCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);

  void OnRegister();
  void ShowStep(OnboardingStep step);
  void LoadIdentityFields();
  void Finish();
  void Mount();
  void DirtyAll();

  OnboardingPorts ports_;
  bool active_ = false;
  OnboardingStep step_ = OnboardingStep::None;
  ui::String step_key_;
  ui::String nickname_;
  ui::String account_id_;
  ui::String error_;
  bool busy_ = false;
};

} // namespace pbr
