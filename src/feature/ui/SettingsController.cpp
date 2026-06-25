#include "feature/ui/SettingsController.h"

#include "base/data/AppPaths.h"
#include "base/data/LlmPreset.h"
#include "base/data/SessionStore.h"
#include "base/platform/BrowserThread.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellHost.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <SDL3/SDL.h>

namespace pbr {

namespace {

constexpr uint64_t kDebounceMs = 500;
constexpr uint64_t kToastSuppressMs = 2000;

Rml::String EventValue(Rml::Event& ev) {
  return ev.GetParameter<Rml::String>("value", Rml::String());
}

const char* BlockName(SettingsController::SettingsBlock block) {
  switch (block) {
  case SettingsController::SettingsBlock::Llm:
    return "llm";
  case SettingsController::SettingsBlock::Appearance:
    return "appearance";
  }
  return "unknown";
}

} // namespace

SettingsController::SettingsController() {
  redirectLogger("SettingsController");
  InitSections();
}

SettingsController& SettingsController::Instance() {
  static SettingsController controller;
  return controller;
}

void SettingsController::InitSections() {
  sections_ = {
      {.id = "llm", .title = "LLM", .subtitle = "Model, endpoint, API key"},
      {.id = "appearance", .title = "Appearance", .subtitle = "Light, dark, or system theme"},
      {.id = "storage", .title = "Storage", .subtitle = "Profile and data paths"},
  };
}

void SettingsController::SyncBindingsFromSession() {
  const BootstrapResult& bootstrap = SessionStore::Instance().Snapshot();
  const AppConfig& config = bootstrap.config;

  draft_.llm_preset = ResolvePreset(config);
  draft_.llm_base_url = config.llm.base_url;
  draft_.llm_model = config.llm.model;
  draft_.llm_api_key = config.llm.api_key;
  draft_.llm_api_key_env = config.llm_api_key_env;
  draft_.appearance = bootstrap.profile_prefs.appearance;

  display_.profile_label = bootstrap.profile_registry.ActiveProfileId();
  display_.config_dir = AppPaths::ConfigDir();
  display_.data_dir = bootstrap.data_dir;
  display_.profile_dir = bootstrap.profile_data_dir;
}

void SettingsController::ReloadFromDisk() {
  if (auto reloaded = SessionStore::Instance().ReloadFromDisk(); !reloaded) {
    status_ = reloaded.error().message;
    log().warning << "ReloadFromDisk failed: " << status_.c_str();
    return;
  }
  status_ = "";
  SyncBindingsFromSession();
}

bool SettingsController::RegisterModel(Rml::Context* context) {
  if (!context) {
    return false;
  }
  context_ = context;

  return DataModelHost::Instance().Register(context, "settings", [](Rml::DataModelConstructor& ctor) {
    auto& controller = SettingsController::Instance();
    if (auto section_handle = ctor.RegisterStruct<SectionListRow>()) {
      section_handle.RegisterMember("id", &SectionListRow::id);
      section_handle.RegisterMember("title", &SectionListRow::title);
      section_handle.RegisterMember("subtitle", &SectionListRow::subtitle);
    }
    ctor.RegisterArray<std::vector<SectionListRow>>();
    ctor.Bind("sections", &controller.sections_);
    ctor.Bind("selected_id", &controller.selected_id_);
    ctor.Bind("selected_title", &controller.selected_title_);
    ctor.Bind("compact_layout", &controller.compact_layout_);
    ctor.Bind("show_detail", &controller.show_detail_);
    ctor.Bind("llm_preset", &controller.draft_.llm_preset);
    ctor.Bind("llm_base_url", &controller.draft_.llm_base_url);
    ctor.Bind("llm_model", &controller.draft_.llm_model);
    ctor.Bind("llm_api_key", &controller.draft_.llm_api_key);
    ctor.Bind("llm_api_key_env", &controller.draft_.llm_api_key_env);
    ctor.Bind("appearance", &controller.draft_.appearance);
    ctor.Bind("profile_label", &controller.display_.profile_label);
    ctor.Bind("config_dir", &controller.display_.config_dir);
    ctor.Bind("data_dir", &controller.display_.data_dir);
    ctor.Bind("profile_dir", &controller.display_.profile_dir);
    ctor.Bind("status", &controller.status_);
    ctor.BindEventCallback("select_section", &SettingsController::SelectSectionCallback);
    ctor.BindEventCallback("back_to_list", &SettingsController::BackToListCallback);
    ctor.BindEventCallback("reset_section", &SettingsController::ResetSectionCallback);
    ctor.BindEventCallback("on_llm_field_changed", &SettingsController::OnLlmFieldChangedCallback);
    ctor.BindEventCallback("on_llm_preset_changed", &SettingsController::OnLlmPresetChangedCallback);
    ctor.BindEventCallback("on_appearance_changed", &SettingsController::OnAppearanceChangedCallback);
  });
}

void SettingsController::DirtyAll() {
  auto& host = DataModelHost::Instance();
  host.Dirty("settings", "sections");
  host.Dirty("settings", "selected_id");
  host.Dirty("settings", "selected_title");
  host.Dirty("settings", "compact_layout");
  host.Dirty("settings", "show_detail");
  host.Dirty("settings", "llm_preset");
  host.Dirty("settings", "llm_base_url");
  host.Dirty("settings", "llm_model");
  host.Dirty("settings", "llm_api_key");
  host.Dirty("settings", "llm_api_key_env");
  host.Dirty("settings", "appearance");
  host.Dirty("settings", "profile_label");
  host.Dirty("settings", "config_dir");
  host.Dirty("settings", "data_dir");
  host.Dirty("settings", "profile_dir");
  host.Dirty("settings", "status");
}

void SettingsController::OnSettingsMounted() {
  suppress_auto_save_ = true;
  DirtyAll();
  if (context_) {
    context_->Update();
  }
  suppress_auto_save_ = false;
}

void SettingsController::OnNavTabActivated() {
  log().info << "OnNavTabActivated";
  pending_flush_block_.reset();
  pending_flush_at_ms_ = 0;
  show_detail_ = false;
  selected_id_.clear();
  selected_title_.clear();
  compact_layout_ = ShellHost::Instance().State().layout_mode == LayoutMode::Compact;
  ReloadFromDisk();
  OnSettingsMounted();
}

void SettingsController::OnNavTabDeactivated() {
  FlushPending();
}

void SettingsController::SyncLayoutMode() {
  const bool compact = ShellHost::Instance().State().layout_mode == LayoutMode::Compact;
  if (compact_layout_ == compact) {
    return;
  }

  log().info << "SyncLayoutMode compact=" << compact;
  suppress_auto_save_ = true;
  compact_layout_ = compact;
  if (!compact) {
    show_detail_ = false;
    if (!selected_id_.empty()) {
      ShellHost::Instance().SetPrimaryPane("settings_detail");
      BrowserThread::RunUITasks();
    }
  } else if (!selected_id_.empty()) {
    show_detail_ = true;
    ShellHost::Instance().ClearPrimaryPane();
    BrowserThread::RunUITasks();
  }
  DirtyAll();
  suppress_auto_save_ = false;
}

void SettingsController::OpenSettings() {
  const bool already_settings = ShellHost::Instance().State().nav_tab == NavTab::Settings;
  ShellHost::Instance().SelectNavTab(NavTab::Settings);
  if (already_settings) {
    OnNavTabActivated();
  }
}

SettingsDraft SettingsController::ToLogicDraft() const {
  SettingsDraft draft;
  draft.llm_preset = draft_.llm_preset.c_str();
  draft.llm_base_url = draft_.llm_base_url.c_str();
  draft.llm_model = draft_.llm_model.c_str();
  draft.llm_api_key = draft_.llm_api_key.c_str();
  draft.llm_api_key_env = draft_.llm_api_key_env.c_str();
  return draft;
}

void SettingsController::ScheduleBlockFlush(SettingsBlock block) {
  if (suppress_auto_save_) {
    log().info << "ScheduleBlockFlush(" << BlockName(block) << ") suppressed during UI transition";
    return;
  }
  pending_flush_block_ = block;
  pending_flush_at_ms_ = SDL_GetTicks() + kDebounceMs;
}

void SettingsController::FlushPending() {
  if (!pending_flush_block_) {
    return;
  }
  const SettingsBlock block = *pending_flush_block_;
  pending_flush_block_.reset();
  pending_flush_at_ms_ = 0;
  FlushBlock(block);
}

void SettingsController::Tick() {
  if (!pending_flush_block_ || pending_flush_at_ms_ == 0) {
    return;
  }
  if (SDL_GetTicks() >= pending_flush_at_ms_) {
    FlushPending();
  }
}

bool SettingsController::FlushBlock(SettingsBlock block) {
  if (suppress_auto_save_) {
    log().info << "FlushBlock(" << BlockName(block) << ") suppressed during UI transition";
    return true;
  }

  pending_flush_block_.reset();
  pending_flush_at_ms_ = 0;

  log().info << "FlushBlock(" << BlockName(block) << ")";

  if (block == SettingsBlock::Llm) {
    const AppConfig config =
        ApplySettingsDraft(SessionStore::Instance().Snapshot().config, ToLogicDraft());

    if (auto saved = SessionStore::Instance().SaveConfig(config); !saved) {
      status_ = saved.error().message;
      DataModelHost::Instance().Dirty("settings", "status");
      return false;
    }
  } else if (block == SettingsBlock::Appearance) {
    ProfilePreferences profile_prefs = SessionStore::Instance().Snapshot().profile_prefs;
    profile_prefs.appearance = draft_.appearance.c_str();
    if (auto saved = SessionStore::Instance().SaveProfilePrefs(profile_prefs); !saved) {
      status_ = saved.error().message;
      DataModelHost::Instance().Dirty("settings", "status");
      return false;
    }
  }

  SyncBindingsFromSession();
  status_ = "";
  DirtyAll();
  MaybeShowSaveToast(block);
  ShellHost::Instance().DirtyWindow();
  return true;
}

void SettingsController::MaybeShowSaveToast(SettingsBlock block) {
  const uint64_t now = SDL_GetTicks();
  if (last_toast_block_ == block && now - last_toast_at_ms_ < kToastSuppressMs) {
    return;
  }
  last_toast_block_ = block;
  last_toast_at_ms_ = now;
  ShellFeedback::ShowToast(ShellHost::Instance().State(), "Settings saved");
}

void SettingsController::OnResetSection(const std::string& section_id) {
  if (section_id == "llm") {
    const AppConfig defaults = SessionStore::Instance().DefaultConfig();
    draft_.llm_preset = ResolvePreset(defaults);
    draft_.llm_base_url = defaults.llm.base_url;
    draft_.llm_model = defaults.llm.model;
    draft_.llm_api_key = defaults.llm.api_key;
    draft_.llm_api_key_env = defaults.llm_api_key_env;
    DirtyAll();
    FlushBlock(SettingsBlock::Llm);
    return;
  }

  if (section_id == "appearance") {
    draft_.appearance = SessionStore::Instance().DefaultProfilePrefs().appearance;
    DirtyAll();
    FlushBlock(SettingsBlock::Appearance);
  }
}

void SettingsController::CompleteSectionSelection(bool expanded) {
  suppress_auto_save_ = true;
  if (expanded) {
    ShellHost::Instance().SetPrimaryPane("settings_detail");
    BrowserThread::RunUITasks();
  }
  DirtyAll();
  if (context_) {
    context_->Update();
  }
  suppress_auto_save_ = false;
  log().info << "CompleteSectionSelection id=" << selected_id_.c_str()
             << " expanded=" << expanded;
}

void SettingsController::OnSelectSection(const std::string& section_id) {
  log().info << "OnSelectSection(" << section_id << ")";
  FlushPending();

  selected_id_.clear();
  selected_title_.clear();
  for (const SectionListRow& section : sections_) {
    if (section.id == section_id.c_str()) {
      selected_id_ = section.id;
      selected_title_ = section.title;
      break;
    }
  }

  status_ = "";
  compact_layout_ = ShellHost::Instance().State().layout_mode == LayoutMode::Compact;
  if (compact_layout_) {
    show_detail_ = true;
    BrowserThread::PostTask(BrowserThreadId::UI, [this]() { CompleteSectionSelection(false); });
  } else {
    show_detail_ = false;
    BrowserThread::PostTask(BrowserThreadId::UI, [this]() { CompleteSectionSelection(true); });
  }
}

void SettingsController::OnBackToList() {
  log().info << "OnBackToList";
  FlushPending();
  show_detail_ = false;
  selected_id_.clear();
  selected_title_.clear();
  status_ = "";
  BrowserThread::PostTask(BrowserThreadId::UI, [this]() {
    suppress_auto_save_ = true;
    DirtyAll();
    if (context_) {
      context_->Update();
    }
    suppress_auto_save_ = false;
  });
}

void SettingsController::SelectSectionCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                const Rml::VariantList& args) {
  if (args.empty()) {
    SettingsController::Instance().log().warning << "select_section called with no args";
    return;
  }
  if (args[0].GetType() != Rml::Variant::STRING) {
    SettingsController::Instance().log().warning << "select_section arg type="
                                              << static_cast<int>(args[0].GetType());
    return;
  }
  Instance().OnSelectSection(std::string(args[0].Get<Rml::String>().c_str()));
}

void SettingsController::BackToListCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& /*args*/) {
  Instance().OnBackToList();
}

void SettingsController::ResetSectionCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                              const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().OnResetSection(std::string(args[0].Get<Rml::String>().c_str()));
}

void SettingsController::OnLlmFieldChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                   const Rml::VariantList& /*args*/) {
  Instance().ScheduleBlockFlush(SettingsBlock::Llm);
}

void SettingsController::OnLlmPresetChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                    const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  if (controller.suppress_auto_save_) {
    return;
  }
  controller.draft_.llm_preset = EventValue(ev);
  controller.FlushBlock(SettingsBlock::Llm);
}

void SettingsController::OnAppearanceChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                       const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  if (controller.suppress_auto_save_) {
    return;
  }
  controller.draft_.appearance = EventValue(ev);
  controller.FlushBlock(SettingsBlock::Appearance);
}

} // namespace pbr
