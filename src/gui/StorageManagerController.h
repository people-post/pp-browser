#pragma once

#include "feature/settings/SettingsCommands.h"
#include "feature/settings/StorageManagerModel.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "common/Lifetime.h"

#include <ui/base/Types.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace ui {
class DataModelConstructor;
}

namespace pbr {

/**
 * Me → Storage (DEV/PLAN-storage-manager.md): the usage bar on the first page, "Manage storage" (chats by size,
 * files over 5 MB) and one chat's photos / videos / files with select-and-delete. The three pages live in the
 * storage section (`storage_page`: main / manage / items); each deeper page and select mode puts a step on the
 * shell's back stack, so the phone's back (header button, swipe, system back) and Escape go up one step, and the
 * page's own back button does the same on a desktop. Its fields are part of the settings model; the ports are the
 * controller's, read when used.
 *
 * UI thread.
 */
class StorageManagerController {
public:
  struct Segment {
    ui::String kind;
    ui::String width;
    ui::String label;
  };
  struct ChatRow {
    ui::String name;
    ui::String letter;
    ui::String summary;
    ui::String size;
    int tone = 0;
  };
  struct Cell {
    ui::String thumb;
    ui::String size;
    bool video = false;
    bool picked = false;
  };
  struct FileRow {
    ui::String name;
    ui::String detail;
    ui::String size;
    ui::String ext;
    bool picked = false;
  };

  /** `visible`: the storage section shows now (changes are counted again only then). */
  StorageManagerController(const SettingsCommands& commands, const ShellNavigationPorts& navigation,
                           const ShellFeedbackPorts& feedback, std::function<bool()> visible);

  /** Its fields and events in the settings model; also the long press that starts selecting. */
  void Bind(ui::DataModelConstructor& ctor);
  /** The storage section was chosen: back to its first page, nothing selected, everything counted again. */
  void Enter();
  /** Another section was chosen: the deeper pages close and their back steps go. */
  void Reset();
  /** The shell dropped its back stack (phone / desktop layout switch): put this page's steps on it again. */
  void RestoreBackStack();

  void OpenManage();
  void OpenLarge();
  void OpenChat(int index);
  void SetTab(const std::string& kind);
  void TapCell(int index);
  void TapFile(int index);
  void LoadMore();
  /** "Select" / "Cancel". */
  void ToggleSelect();
  /** Long press on a cell (or a file row): selecting starts with it picked. */
  void PickByLongPress(int index);
  /** The page's back button: leaves select mode first, then goes up one page. */
  void Back();
  void DeleteSelected();

  const std::string& Page() const { return page_; }
  const std::string& Tab() const { return tab_; }
  bool Selecting() const { return selecting_; }
  bool Busy() const { return busy_; }
  size_t SelectedCount() const { return selected_.size(); }
  const std::vector<Cell>& Cells() const { return cells_; }
  const std::vector<FileRow>& Files() const { return files_; }
  const std::vector<ChatRow>& Chats() const { return chats_; }
  std::string SelectionLabel() const { return selection_label_.c_str(); }

private:
  struct Counts;

  void OnChanged();
  void OnThumbnailReady(const std::string& thread_id, const std::vector<uint8_t>& content_hash);
  void Follow(StorageRecomputeGate::Decision decision);
  void StartRecompute();
  void ApplyCounts(Counts counts);
  void BuildOverviewRows();
  void EnterItems();
  void LeaveItems();
  void NewGeneration();
  void BuildPageItems();
  void BuildCells();
  void RefreshThumbnails();
  void TogglePicked(int index);
  void SelectionChanged();
  void UpdateSelectionLabel();
  void EnterSelect();
  void ExitSelect(bool clear_back_step);
  void OpenItem(const StoredAttachmentItem& item);
  void PerformDelete(std::vector<StoredAttachmentRef> refs);
  void PushBackStep(const char* id, std::function<void()> commit);
  void ClearBackStep(const char* id);
  std::vector<StoredAttachmentItem> SelectedItems() const;
  void DirtyAll();

  const SettingsCommands& commands_;
  const ShellNavigationPorts& navigation_;
  const ShellFeedbackPorts& feedback_;
  std::function<bool()> visible_;
  pp::Lifetime lifetime_;
  bool provider_registered_ = false;
  bool on_changed_installed_ = false;
  bool on_thumbnail_ready_installed_ = false;

  // Data.
  std::vector<StoredAttachmentItem> all_items_;
  StorageOverview overview_;
  StorageHashUses hash_uses_; // all_items_ by hash: a selection is weighed from it
  StorageUsage usage_;
  std::vector<StorageChatName> names_;
  std::vector<std::string> chat_ids_;  // chats_ order
  StorageRecomputeGate gate_;
  uint64_t refresh_seq_ = 0;

  // Page state.
  std::string page_ = "main";
  std::string tab_ = "picture";
  bool items_large_ = false;
  std::string items_thread_;
  std::vector<StoredAttachmentItem> page_all_;   // the open chat's items (or the large list), every kind
  std::vector<StoredAttachmentItem> tab_items_;  // the open tab's, in display order
  size_t shown_ = kStoragePageSize;
  uint64_t generation_ = 0;
  std::set<std::string> requested_;  // thumbnails asked for in this generation
  std::set<std::string> selected_;   // content hashes on the open page
  bool selecting_ = false;
  bool busy_ = false;
  std::optional<uint64_t> releasable_;
  std::set<std::string> opening_;

  // Bound fields.
  bool ready_ = false;
  ui::String total_label_;
  std::vector<Segment> segments_;
  ui::String manage_label_;
  std::vector<Segment> manage_segments_;
  bool large_visible_ = false;
  ui::String large_count_label_;
  ui::String large_size_label_;
  std::vector<ChatRow> chats_;
  bool media_empty_ = false;
  ui::String items_title_;
  ui::String tab_picture_label_;
  ui::String tab_video_label_;
  ui::String tab_file_label_;
  std::vector<Cell> cells_;
  std::vector<FileRow> files_;
  bool list_empty_ = false;
  bool has_more_ = false;
  bool has_selection_ = false;
  ui::String selection_label_;
  ui::String page_binding_ = "main";
  ui::String tab_binding_ = "picture";
};

} // namespace pbr
