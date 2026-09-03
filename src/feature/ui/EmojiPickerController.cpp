#include "feature/ui/EmojiPickerController.h"

#include "foundation/runtime/AppRuntime.h"
#include "domain/ui/ShellTypes.h"
#include "common/EmojiKey.h"
#include "feature/ui/shell/DataModelHost.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <stdexcept>

namespace pbr {
namespace {

/** Prefetch / keep this many sections ahead of and behind the active category. */
constexpr int kWindowSectionSpan = 4;

} // namespace

EmojiPickerController* EmojiPickerController::installed_instance_ = nullptr;

EmojiPickerController::EmojiPickerController() = default;

void EmojiPickerController::InstallInstance(EmojiPickerController& controller) {
  installed_instance_ = &controller;
}

void EmojiPickerController::ClearInstance() {
  installed_instance_ = nullptr;
}

EmojiPickerController& EmojiPickerController::Instance() {
  if (!installed_instance_) {
    throw std::runtime_error("EmojiPickerController not installed");
  }
  return *installed_instance_;
}

void EmojiPickerController::BindShellNavigation(ShellNavigationPorts ports) {
  shell_navigation_ = std::move(ports);
}

void EmojiPickerController::BindShellFeedback(ShellFeedbackPorts ports) {
  shell_feedback_ = std::move(ports);
}

void EmojiPickerController::BindFlowCoordinator(FlowCoordinatorPorts ports) {
  flow_coordinator_ = std::move(ports);
}

void EmojiPickerController::BindSessionStore(SessionStore& store) {
  session_store_ = &store;
}

bool EmojiPickerController::RegisterModel(Rml::Context* context) {
  if (!context) {
    return false;
  }
  context_ = context;

  return DataModelHost::Instance().Register(context, "emoji_picker", [this](Rml::DataModelConstructor& ctor) {
    auto& controller = *this;
    if (auto cell = ctor.RegisterStruct<Cell>()) {
      cell.RegisterMember("glyph", &Cell::glyph);
    }
    // Register array element types before struct members that hold those arrays.
    ctor.RegisterArray<std::vector<Cell>>();
    if (auto section = ctor.RegisterStruct<Section>()) {
      section.RegisterMember("id", &Section::id);
      section.RegisterMember("element_id", &Section::element_id);
      section.RegisterMember("label", &Section::label);
      section.RegisterMember("cells", &Section::cells);
    }
    ctor.RegisterArray<std::vector<Section>>();
    if (auto tab = ctor.RegisterStruct<RailTab>()) {
      tab.RegisterMember("id", &RailTab::id);
      tab.RegisterMember("glyph", &RailTab::glyph);
      tab.RegisterMember("active", &RailTab::active);
    }
    ctor.RegisterArray<std::vector<RailTab>>();
    ctor.Bind("title", &controller.title_);
    ctor.Bind("active_category", &controller.active_category_);
    ctor.Bind("rail_tabs", &controller.rail_tabs_);
    ctor.Bind("sections", &controller.sections_);
    ctor.BindEventCallback("select_emoji", &EmojiPickerController::SelectEmojiCallback);
    ctor.BindEventCallback("select_category", &EmojiPickerController::SelectCategoryCallback);
    ctor.BindEventCallback("on_scroll", &EmojiPickerController::OnScrollCallback);
    ctor.BindEventCallback("cancel_picker", &EmojiPickerController::CancelCallback);
  });
}

void EmojiPickerController::OpenInsert(
    std::function<void(std::string emoji, bool restore_composer_focus)> on_pick) {
  // Toggle: ☺ again while bottom chrome emoji is open closes it (no OSK bounce).
  if (IsBottomChromeEmojiOpen()) {
    Close();
    return;
  }
  if (layer_id_ >= 0) {
    Close();
  }
  mode_ = Mode::Insert;
  react_message_id_.clear();
  on_insert_pick_ = std::move(on_pick);
  on_react_pick_ = nullptr;
  title_ = "Insert emoji";
  active_category_.clear();
  RebuildModel();
  DirtyAll();

  const bool prefer_bottom =
      shell_navigation_.uses_bottom_chrome && shell_navigation_.uses_bottom_chrome();
  if (prefer_bottom) {
    OpenBottomChromePresentation();
    if (bottom_chrome_mode_) {
      return;
    }
  }
  OpenOverlayPresentation();
}

void EmojiPickerController::OpenReact(std::string message_id,
                                      std::function<void(std::string emoji)> on_pick) {
  if (IsBottomChromeEmojiOpen()) {
    Close();
  }
  if (layer_id_ >= 0) {
    Close();
  }
  mode_ = Mode::React;
  react_message_id_ = std::move(message_id);
  on_react_pick_ = std::move(on_pick);
  on_insert_pick_ = nullptr;
  title_ = "React";
  active_category_.clear();
  RebuildModel();
  DirtyAll();
  OpenOverlayPresentation();
}

bool EmojiPickerController::IsBottomChromeEmojiOpen() const {
  if (bottom_chrome_mode_) {
    return true;
  }
  return shell_navigation_.bottom_chrome_open && shell_navigation_.bottom_chrome_open();
}

void EmojiPickerController::OpenBottomChromePresentation() {
  if (!shell_navigation_.set_bottom_chrome) {
    bottom_chrome_mode_ = false;
    return;
  }
  BottomChromeSpec spec;
  spec.key = "emoji_picker";
  bottom_chrome_mode_ = shell_navigation_.set_bottom_chrome(spec);
}

void EmojiPickerController::OpenOverlayPresentation() {
  bottom_chrome_mode_ = false;
  PaneSpec spec;
  spec.key = "emoji_picker";
  if (mode_ == Mode::Insert) {
    spec.return_focus_id = "draft-input";
  }
  layer_id_ = shell_navigation_.push_layer ? shell_navigation_.push_layer(spec) : -1;
  RegisterFlow();
}

void EmojiPickerController::RegisterFlow() {
  if (!flow_coordinator_.begin_modal) {
    return;
  }
  flow_coordinator_.begin_modal(
      layer_id_, []() { return false; },
      [this]() { OnFlowDismissed(); });
}

void EmojiPickerController::OnFlowDismissed() {
  layer_id_ = -1;
  ResetState();
  DirtyAll();
}

void EmojiPickerController::Close() {
  const bool was_bottom = bottom_chrome_mode_;
  const int closing_id = layer_id_;
  layer_id_ = -1;
  ResetState();
  if (was_bottom && shell_navigation_.clear_bottom_chrome) {
    shell_navigation_.clear_bottom_chrome();
  }
  if (flow_coordinator_.end_modal) {
    flow_coordinator_.end_modal();
  }
  if (closing_id >= 0 && shell_navigation_.close_layer) {
    shell_navigation_.close_layer(closing_id);
  }
  DirtyAll();
}

void EmojiPickerController::ResetState() {
  on_insert_pick_ = nullptr;
  on_react_pick_ = nullptr;
  bottom_chrome_mode_ = false;
  react_message_id_.clear();
  rail_tabs_.clear();
  sections_.clear();
  active_category_.clear();
  window_begin_ = 0;
  window_end_ = 0;
  pending_scroll_height_before_.reset();
  pending_scroll_top_before_.reset();
}

void EmojiPickerController::ComputeSectionWindow(int center_index, int section_count, int span,
                                                 int prev_end, int& begin_out, int& end_out) {
  if (section_count <= 0 || span <= 0) {
    begin_out = 0;
    end_out = 0;
    return;
  }
  const int use_span = std::min(span, section_count);
  // Grow end ahead of center and never shrink — empty sections below have no height, so
  // scroll-driven load requires the window to expand as the user moves down.
  const int desired_end = std::min(section_count, std::max(use_span, center_index + use_span));
  end_out = std::max(prev_end, desired_end);
  // Unload only well above the active category (behind the viewport).
  begin_out = std::max(0, center_index - use_span);
  if (begin_out > end_out) {
    begin_out = std::max(0, end_out - use_span);
  }
}

void EmojiPickerController::EnsureWindowAround(int center_index) {
  std::vector<std::string> recent;
  if (session_store_) {
    recent = session_store_->Snapshot().profile_prefs.recent_emojis;
  }
  const std::vector<EmojiCategory> cats = EmojiCatalog::BuildWithRecent(recent);
  if (cats.empty() || sections_.size() != cats.size()) {
    return;
  }
  const int n = static_cast<int>(cats.size());
  const int prev_begin = window_begin_;
  ComputeSectionWindow(center_index, n, kWindowSectionSpan, window_end_, window_begin_, window_end_);

  Rml::Element* body = FindScrollBody();
  const bool unloading_above = window_begin_ > prev_begin;
  if (unloading_above && body) {
    pending_scroll_height_before_ = body->GetScrollHeight();
    pending_scroll_top_before_ = body->GetScrollTop();
  }

  for (int i = 0; i < n; ++i) {
    Section& section = sections_[static_cast<size_t>(i)];
    if (i >= window_begin_ && i < window_end_) {
      if (section.cells.empty()) {
        section.cells.reserve(cats[static_cast<size_t>(i)].glyphs.size());
        for (const std::string& g : cats[static_cast<size_t>(i)].glyphs) {
          Cell cell;
          cell.glyph = g.c_str();
          section.cells.push_back(std::move(cell));
        }
      }
    } else if (!section.cells.empty()) {
      section.cells.clear();
      section.cells.shrink_to_fit();
    }
  }
}

void EmojiPickerController::ApplyPendingScrollAdjust() {
  if (!pending_scroll_height_before_.has_value() || !pending_scroll_top_before_.has_value()) {
    return;
  }
  Rml::Element* body = FindScrollBody();
  if (!body) {
    pending_scroll_height_before_.reset();
    pending_scroll_top_before_.reset();
    return;
  }
  const float delta = *pending_scroll_height_before_ - body->GetScrollHeight();
  body->SetScrollTop(std::max(0.f, *pending_scroll_top_before_ - delta));
  pending_scroll_height_before_.reset();
  pending_scroll_top_before_.reset();
}

void EmojiPickerController::RebuildModel() {
  std::vector<std::string> recent;
  if (session_store_) {
    recent = session_store_->Snapshot().profile_prefs.recent_emojis;
  }
  const std::vector<EmojiCategory> cats = EmojiCatalog::BuildWithRecent(recent);

  rail_tabs_.clear();
  sections_.clear();
  window_begin_ = 0;
  window_end_ = 0;
  pending_scroll_height_before_.reset();
  pending_scroll_top_before_.reset();
  rail_tabs_.reserve(cats.size());
  sections_.reserve(cats.size());

  for (const EmojiCategory& cat : cats) {
    RailTab tab;
    tab.id = cat.id.c_str();
    tab.glyph = cat.rail_glyph.c_str();
    tab.active = false;
    rail_tabs_.push_back(std::move(tab));

    Section section;
    section.id = cat.id.c_str();
    section.element_id = ("emoji-section-" + cat.id).c_str();
    section.label = cat.label.c_str();
    sections_.push_back(std::move(section));
  }

  if (active_category_.empty() && !cats.empty()) {
    active_category_ = cats.front().id.c_str();
  }

  int active_index = 0;
  for (int i = 0; i < static_cast<int>(cats.size()); ++i) {
    if (cats[static_cast<size_t>(i)].id == active_category_.c_str()) {
      active_index = i;
      break;
    }
  }
  for (int i = 0; i < static_cast<int>(rail_tabs_.size()); ++i) {
    rail_tabs_[static_cast<size_t>(i)].active = (i == active_index);
  }
  EnsureWindowAround(active_index);
}

void EmojiPickerController::DirtyAll() {
  DataModelHost::Instance().Dirty("emoji_picker", "title");
  DataModelHost::Instance().Dirty("emoji_picker", "active_category");
  DataModelHost::Instance().Dirty("emoji_picker", "rail_tabs");
  DataModelHost::Instance().Dirty("emoji_picker", "sections");
}

void EmojiPickerController::PersistRecent(const std::string& glyph) {
  if (!session_store_) {
    return;
  }
  ProfilePreferences prefs = session_store_->Snapshot().profile_prefs;
  prefs.schema_version = ProfilePreferences::kSchemaVersion;
  EmojiCatalog::TouchRecent(prefs.recent_emojis, glyph);
  (void)session_store_->SaveProfilePrefs(prefs);
}

void EmojiPickerController::OnEmojiPicked(const std::string& glyph) {
  const std::string key = NormalizeEmojiKey(glyph);
  if (key.empty()) {
    return;
  }
  PersistRecent(key);
  if (mode_ == Mode::Insert) {
    // Bottom-chrome Insert stays open for multi-tap (OSK stays dismissed).
    // Expanded popover Insert closes and restores composer focus via return_focus_id.
    if (bottom_chrome_mode_) {
      if (on_insert_pick_) {
        on_insert_pick_(key, /*restore_composer_focus=*/false);
      }
      return;
    }
    auto pick = std::move(on_insert_pick_);
    Close();
    if (pick) {
      pick(key, /*restore_composer_focus=*/true);
    }
    return;
  }
  auto pick = std::move(on_react_pick_);
  Close();
  if (pick) {
    pick(key);
  }
}

Rml::Element* EmojiPickerController::FindScrollBody() const {
  if (!context_) {
    return nullptr;
  }
  for (int i = 0; i < context_->GetNumDocuments(); ++i) {
    Rml::ElementDocument* d = context_->GetDocument(i);
    if (!d) {
      continue;
    }
    if (Rml::Element* el = d->GetElementById("emoji-picker-body")) {
      return el;
    }
  }
  return nullptr;
}

Rml::Element* EmojiPickerController::FindSectionElement(const std::string& category_id) const {
  if (!context_) {
    return nullptr;
  }
  const Rml::String id = ("emoji-section-" + category_id).c_str();
  for (int i = 0; i < context_->GetNumDocuments(); ++i) {
    Rml::ElementDocument* d = context_->GetDocument(i);
    if (!d) {
      continue;
    }
    if (Rml::Element* el = d->GetElementById(id)) {
      return el;
    }
  }
  return nullptr;
}

void EmojiPickerController::UpdateActiveFromScroll() {
  Rml::Element* body = FindScrollBody();
  if (!body || sections_.empty()) {
    return;
  }
  const float probe_y = body->GetAbsoluteTop() + 8.f;

  std::string best = sections_.front().id.c_str();
  for (const Section& section : sections_) {
    Rml::Element* el = FindSectionElement(section.id.c_str());
    if (!el) {
      continue;
    }
    if (el->GetAbsoluteTop() <= probe_y) {
      best = section.id.c_str();
    }
  }

  int active_index = 0;
  for (int i = 0; i < static_cast<int>(sections_.size()); ++i) {
    if (sections_[static_cast<size_t>(i)].id.c_str() == best) {
      active_index = i;
      break;
    }
  }

  const bool category_changed = (best != active_category_.c_str());
  if (category_changed) {
    active_category_ = best.c_str();
  }
  for (int i = 0; i < static_cast<int>(rail_tabs_.size()); ++i) {
    rail_tabs_[static_cast<size_t>(i)].active = (i == active_index);
  }

  const int prev_begin = window_begin_;
  const int prev_end = window_end_;
  EnsureWindowAround(active_index);

  if (category_changed) {
    DataModelHost::Instance().Dirty("emoji_picker", "rail_tabs");
    DataModelHost::Instance().Dirty("emoji_picker", "active_category");
  }
  if (window_begin_ != prev_begin || window_end_ != prev_end) {
    DataModelHost::Instance().Dirty("emoji_picker", "sections");
    // Unloading above shrinks scroll height — restore after the next layout pass.
    if (pending_scroll_height_before_.has_value()) {
      AppRuntime::PostUI([]() { EmojiPickerController::Instance().ApplyPendingScrollAdjust(); });
    }
  } else if (category_changed) {
    DataModelHost::Instance().Dirty("emoji_picker", "rail_tabs");
  }
}

void EmojiPickerController::ScrollToCategory(const std::string& category_id) {
  int target = -1;
  for (int i = 0; i < static_cast<int>(sections_.size()); ++i) {
    if (sections_[static_cast<size_t>(i)].id.c_str() == category_id) {
      target = i;
      break;
    }
  }
  if (target < 0) {
    return;
  }
  active_category_ = category_id.c_str();
  for (int i = 0; i < static_cast<int>(rail_tabs_.size()); ++i) {
    rail_tabs_[static_cast<size_t>(i)].active = (i == target);
  }
  EnsureWindowAround(target);
  DirtyAll();
  if (pending_scroll_height_before_.has_value()) {
    AppRuntime::PostUI([]() { EmojiPickerController::Instance().ApplyPendingScrollAdjust(); });
  }

  if (Rml::Element* el = FindSectionElement(category_id)) {
    el->ScrollIntoView(true);
  }
}

void EmojiPickerController::SelectEmojiCallback(Rml::DataModelHandle, Rml::Event&,
                                                const Rml::VariantList& args) {
  if (args.empty()) {
    return;
  }
  Instance().OnEmojiPicked(args[0].Get<Rml::String>().c_str());
}

void EmojiPickerController::SelectCategoryCallback(Rml::DataModelHandle, Rml::Event&,
                                                   const Rml::VariantList& args) {
  if (args.empty()) {
    return;
  }
  Instance().ScrollToCategory(args[0].Get<Rml::String>().c_str());
}

void EmojiPickerController::OnScrollCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
  Instance().UpdateActiveFromScroll();
}

void EmojiPickerController::CancelCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
  Instance().Close();
}

} // namespace pbr
