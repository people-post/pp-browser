#pragma once

#include "foundation/data/SessionStore.h"
#include "lib/ui/EmojiCatalog.h"
#include "common/Module.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "gui/shell/ShellNavigationPorts.h"

#include <ui/base/ObserverPtr.h>
#include <ui/data/DataModelHandle.h>
#include <ui/dom/Event.h>
#include <ui/dom/EventListener.h>
#include <ui/base/Types.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace ui {
class Context;
class Element;
} // namespace ui

namespace pbr {

class EmojiPickerController : public Module {
public:
  enum class Mode { Insert, React };

  struct RailTab {
    ui::String id;
    ui::String glyph;
    bool active = false;
  };

  struct Cell {
    ui::String glyph;
  };

  struct Section {
    ui::String id;
    ui::String element_id;
    ui::String label;
    std::vector<Cell> cells;
  };

  EmojiPickerController();
  ~EmojiPickerController() override = default;

  static void InstallInstance(EmojiPickerController& controller);
  static void ClearInstance();
  static EmojiPickerController& Instance();

  void BindShellNavigation(ShellNavigationPorts ports);
  void BindShellFeedback(ShellFeedbackPorts ports);
  void BindFlowCoordinator(FlowCoordinatorPorts ports);
  void BindSessionStore(SessionStore& store);

  bool RegisterModel(ui::Context* context);

  void OpenInsert(std::function<void(std::string emoji, bool restore_composer_focus)> on_pick);
  /** On phones a page sheet that opens at about a third of the screen (Kenneth 2026-11-05 原话 81). */
  void OpenReact(std::string message_id, std::function<void(std::string emoji)> on_pick);
  void Close();
  /**
   * The phone composer's emoji sheet: closes it and brings the keyboard up in the message field (its X, a drag down
   * on its top, the emoji button again, a tap in the field). A tap on the transcript only closes it (Close).
   */
  void CloseToKeyboard();
  /** Shown as an overlay or as the keyboard-area panel. */
  bool IsOpen() const { return layer_id_ >= 0 || bottom_chrome_mode_; }

  /**
   * Grow-ahead + unload-behind window for emoji sections.
   * `end` never shrinks below `prev_end` so scroll can reach categories below.
   * `begin` tracks behind `center` so far-above sections can unload.
   */
  static void ComputeSectionWindow(int center_index, int section_count, int span, int prev_end, int& begin_out,
                                   int& end_out);

private:
  void OpenOverlayPresentation();
  void OpenBottomChromePresentation();
  void RegisterFlow();
  void OnFlowDismissed();
  void ResetState();
  void RebuildModel();
  void DirtyAll();
  void PersistRecent(const std::string& glyph);
  void OnEmojiPicked(const std::string& glyph);
  void UpdateActiveFromScroll();
  void ScrollToCategory(const std::string& category_id);
  void EnsureWindowAround(int center_index);
  void ApplyPendingScrollAdjust();
  bool IsBottomChromeEmojiOpen() const;
  ui::Element* FindScrollBody() const;
  ui::Element* FindSectionElement(const std::string& category_id) const;

  static void SelectEmojiCallback(ui::DataModelHandle, ui::Event&, const ui::VariantList& args);
  static void SelectCategoryCallback(ui::DataModelHandle, ui::Event&, const ui::VariantList& args);
  static void OnScrollCallback(ui::DataModelHandle, ui::Event&, const ui::VariantList&);
  static void CancelCallback(ui::DataModelHandle, ui::Event&, const ui::VariantList&);
  static void CloseToKeyboardCallback(ui::DataModelHandle, ui::Event&, const ui::VariantList&);

  /**
   * While the composer's emoji sheet is open (phone): a press in the message field or a drag down on the sheet's top
   * switches to the keyboard, a press on the transcript closes the sheet. One bubbling listener on the document.
   */
  class SheetTaps : public ui::EventListener {
  public:
    explicit SheetTaps(EmojiPickerController& owner) : owner_(owner) {}
    ~SheetTaps() override { Detach(); }
    void Attach(ui::Element* document);
    void Detach();
    void ProcessEvent(ui::Event& event) override;

  private:
    EmojiPickerController& owner_;
    ui::ObserverPtr<ui::Element> document_;
    /** Where a press on the sheet's top (grabber and title row) started, in px; < 0: none. */
    float drag_start_y_ = -1.f;
  };
  SheetTaps sheet_taps_{*this};

  ui::Context* context_ = nullptr;
  int layer_id_ = -1;
  Mode mode_ = Mode::Insert;
  /** True when Insert used mobile/compact bottom-chrome (IME slot) presentation. */
  bool bottom_chrome_mode_ = false;
  std::string react_message_id_;
  std::function<void(std::string, bool)> on_insert_pick_;
  std::function<void(std::string)> on_react_pick_;

  ui::String title_;
  ui::String active_category_;
  std::vector<RailTab> rail_tabs_;
  std::vector<Section> sections_;
  /** App-side window: cells bound for [window_begin, window_end); grow end on scroll, unload above. */
  int window_begin_ = 0;
  int window_end_ = 0;
  /** Scroll restore after unloading sections above the viewport (height shrink). */
  std::optional<float> pending_scroll_height_before_;
  std::optional<float> pending_scroll_top_before_;

  ShellNavigationPorts shell_navigation_;
  ShellFeedbackPorts shell_feedback_;
  FlowCoordinatorPorts flow_coordinator_;
  SessionStore* session_store_ = nullptr;

  static EmojiPickerController* installed_instance_;
};

} // namespace pbr
