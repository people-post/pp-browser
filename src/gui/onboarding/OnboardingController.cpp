#include "gui/onboarding/OnboardingController.h"

#include "gui/AccountInvite.h"

#include "lib/ui/ViewCatalog.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/UserFeedback.h"
#include "gui/shell/DataModelHost.h"
#include "common/PbrCompat.h"

#include <ui/data/DataModelHandle.h>

namespace pbr {

namespace {

OnboardingController* g_instance = nullptr;

std::string Trimmed(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

} // namespace

OnboardingController& OnboardingController::Instance() {
  return *g_instance;
}

void OnboardingController::InstallInstance(OnboardingController& controller) {
  g_instance = &controller;
}

void OnboardingController::ClearInstance() {
  g_instance = nullptr;
}

void OnboardingController::BindPorts(OnboardingPorts ports) {
  ports_ = std::move(ports);
}

bool OnboardingController::RegisterModel(ui::Context* context) {
  if (!context) {
    return false;
  }
  return DataModelHost::Instance().Register(context, "onboarding", [this](ui::DataModelConstructor& ctor) {
    ctor.Bind("step", &step_key_);
    ctor.Bind("nickname", &nickname_);
    ctor.Bind("account_id", &account_id_);
    ctor.Bind("error", &error_);
    ctor.Bind("busy", &busy_);
    ctor.BindEventCallback("onboarding_register", &OnboardingController::RegisterCallback);
    ctor.BindEventCallback("onboarding_copy_id", &OnboardingController::CopyIdCallback);
    ctor.BindEventCallback("onboarding_share", &OnboardingController::ShareCallback);
    ctor.BindEventCallback("onboarding_next", &OnboardingController::NextCallback);
    ctor.BindEventCallback("onboarding_find_friends", &OnboardingController::FindFriendsCallback);
    ctor.BindEventCallback("onboarding_finish", &OnboardingController::FinishCallback);
  });
}

void OnboardingController::MaybeStart() {
  if (active_ || !ports_.load_identity || !ports_.mount) {
    return;
  }
  const ProfileIdentityView view = ports_.load_identity();
  OnboardingFacts facts;
  facts.identity_ready = view.ready;
  facts.registered = view.registered == "yes";
  facts.completed = ports_.is_completed && ports_.is_completed();
  const OnboardingStep start = OnboardingStartStep(facts);
  if (start == OnboardingStep::None) {
    return;
  }
  LoadIdentityFields();
  error_.clear();
  busy_ = false;
  active_ = true;
  ShowStep(start);
}

void OnboardingController::MarkCompleted() {
  if (ports_.set_completed) {
    ports_.set_completed(true);
  }
  if (active_) {
    active_ = false;
    Mount();
  }
}

void OnboardingController::LoadIdentityFields() {
  if (!ports_.load_identity) {
    return;
  }
  const ProfileIdentityView view = ports_.load_identity();
  if (nickname_.empty() || !view.nickname.empty()) {
    nickname_ = view.nickname.c_str();
  }
  // The id friends type: the same "account ID" Me → Profile shows and shares (relay id; M002 id as fallback).
  account_id_ = (!view.relay_id.empty() ? view.relay_id : view.account_id).c_str();
  contact_locator_ = view.contact_locator;
}

void OnboardingController::ShowStep(const OnboardingStep step) {
  step_ = step;
  step_key_ = OnboardingStepKey(step);
  Mount();
  DirtyAll();
}

void OnboardingController::Mount() {
  if (!ports_.mount) {
    return;
  }
  ports_.mount(active_ ? ViewCatalog::LoadBody("onboarding") : std::string());
}

void OnboardingController::DirtyAll() {
  auto& host = DataModelHost::Instance();
  for (const char* key : {"step", "nickname", "account_id", "error", "busy"}) {
    host.Dirty("onboarding", key);
  }
}

void OnboardingController::Finish() {
  active_ = false;
  step_ = OnboardingStep::None;
  if (ports_.set_completed) {
    ports_.set_completed(true);
  }
  Mount();
}

void OnboardingController::OnRegister() {
  if (busy_) {
    return;
  }
  const std::string nickname = Trimmed(nickname_.c_str());
  if (nickname.empty()) {
    error_ = Tr("onboarding.name_required").c_str();
    DirtyAll();
    return;
  }
  if (!ports_.register_identity) {
    error_ = Tr("onboarding.register_unavailable").c_str();
    DirtyAll();
    return;
  }
  error_.clear();
  busy_ = true;
  DirtyAll();
  auto work = [register_identity = ports_.register_identity, nickname]() { return register_identity(nickname); };
  auto done = [this](Roe<void> result) {
    busy_ = false;
    if (!result) {
      error_ = (Tr("onboarding.register_failed") + " " + result.error().message).c_str();
      DirtyAll();
      return;
    }
    LoadIdentityFields();
    ShowStep(OnboardingStep::Account);
  };
  if (ports_.run_heavy) {
    ports_.run_heavy(std::move(work), std::move(done));
  } else {
    done(work());
  }
}

void OnboardingController::RegisterCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                            const ui::VariantList& /*args*/) {
  Instance().OnRegister();
}

void OnboardingController::CopyIdCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                          const ui::VariantList& /*args*/) {
  auto& self = Instance();
  if (self.account_id_.empty() || !self.ports_.copy_text) {
    return;
  }
  self.ports_.copy_text(self.account_id_.c_str());
  UserFeedback::Ok(Tr("onboarding.id_copied"));
}

void OnboardingController::ShareCallback(ui::DataModelHandle /*model*/, ui::Event& ev,
                                         const ui::VariantList& /*args*/) {
  Instance().OnShare(ShareAnchorFromEvent(ev));
}

void OnboardingController::OnShare(const ShareAnchor anchor) {
  if (account_id_.empty()) {
    return;
  }
  ShareInviteOrCopy(AccountInviteText(nickname_.c_str(), account_id_.c_str(), contact_locator_), ports_.share_text, anchor,
                    ports_.copy_text);
}

void OnboardingController::NextCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                        const ui::VariantList& /*args*/) {
  auto& self = Instance();
  const OnboardingStep next = OnboardingNextStep(self.step_);
  if (next == OnboardingStep::None) {
    self.Finish();
    return;
  }
  self.ShowStep(next);
}

void OnboardingController::FindFriendsCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                               const ui::VariantList& /*args*/) {
  auto& self = Instance();
  self.Finish();
  if (self.ports_.find_friends) {
    self.ports_.find_friends();
  }
}

void OnboardingController::FinishCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                          const ui::VariantList& /*args*/) {
  Instance().Finish();
}

} // namespace pbr
