#include "gui/StorageManagerController.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/PlatformOpenFile.h"
#include "gui/UserFeedback.h"
#include "gui/chat/MessageTimeLabel.h"
#include "gui/shell/DataModelHost.h"
#include "lib/people/AvatarGlyph.h"
#include "lib/ui/ContextMenuHost.h"
#include "lib/ui/DataEvent.h"
#include "common/runtime/AppRuntime.h"

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Element.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>

namespace pbr {

namespace {

constexpr const char* kModel = "settings";
constexpr const char* kStepManage = "storage_manage";
constexpr const char* kStepItems = "storage_items";
constexpr const char* kStepSelect = "storage_select";
/** Opening a stored copy this large takes a moment (it is decrypted first): say so, like the chat does. */
constexpr uint64_t kSlowOpenBytes = 4ull * 1024 * 1024;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t WallNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string HashKey(const std::vector<uint8_t>& hash) {
  return std::string(hash.begin(), hash.end());
}

/** Tr with a `_one` variant for a count of 1 (English "1 item" / "3 items"). */
std::string TrCount(const std::string& key, const size_t count, std::map<std::string, std::string> args = {}) {
  args["count"] = std::to_string(count);
  return Tr(count == 1 ? key + "_one" : key, args);
}

std::string KindLabel(const std::string& kind) {
  return Tr("settings.storage.manage.kind." + kind);
}

std::vector<StorageManagerController::Segment> SegmentRows(const std::vector<StorageBarSegment>& segments) {
  std::vector<StorageManagerController::Segment> rows;
  rows.reserve(segments.size());
  for (const StorageBarSegment& segment : segments) {
    char width[32];
    std::snprintf(width, sizeof(width), "%.2f%%", static_cast<double>(segment.percent));
    rows.push_back({.kind = segment.kind.c_str(),
                    .width = width,
                    .label = (KindLabel(segment.kind) + " " + FormatByteSize(segment.bytes)).c_str()});
  }
  return rows;
}

std::string CountsLine(const StorageKindTotals& totals) {
  std::string line;
  const auto add = [&line](const char* key, const size_t count) {
    if (count == 0) {
      return;
    }
    if (!line.empty()) {
      line += " · ";
    }
    line += Tr(key, {{"count", std::to_string(count)}});
  };
  add("settings.storage.manage.count_picture", totals.pictures);
  add("settings.storage.manage.count_video", totals.videos);
  add("settings.storage.manage.count_file", totals.files);
  return line;
}

/** "PDF", "ZIP": the file name's extension, else the mime subtype, at most four letters. */
std::string FileTypeLabel(const StoredAttachmentItem& item) {
  std::string ext;
  if (const size_t dot = item.filename.rfind('.'); dot != std::string::npos && dot + 1 < item.filename.size()) {
    ext = item.filename.substr(dot + 1);
  } else if (const size_t slash = item.mime.find('/'); slash != std::string::npos) {
    ext = item.mime.substr(slash + 1);
  }
  if (ext.size() > 4) {
    ext.resize(4);
  }
  for (char& c : ext) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return ext;
}

} // namespace

struct StorageManagerController::Counts {
  std::vector<StoredAttachmentItem> items;
  uint64_t message_store_bytes = 0;
  uint64_t profile_bytes = 0;
  int left = 3;
};

StorageManagerController::StorageManagerController(const SettingsCommands& commands,
                                                   const ShellNavigationPorts& navigation,
                                                   const ShellFeedbackPorts& feedback, std::function<bool()> visible)
    : commands_(commands), navigation_(navigation), feedback_(feedback), visible_(std::move(visible)) {}

void StorageManagerController::Bind(ui::DataModelConstructor& ctor) {
  if (auto segment = ctor.RegisterStruct<Segment>()) {
    segment.RegisterMember("kind", &Segment::kind);
    segment.RegisterMember("width", &Segment::width);
    segment.RegisterMember("label", &Segment::label);
  }
  if (auto chat = ctor.RegisterStruct<ChatRow>()) {
    chat.RegisterMember("name", &ChatRow::name);
    chat.RegisterMember("letter", &ChatRow::letter);
    chat.RegisterMember("summary", &ChatRow::summary);
    chat.RegisterMember("size", &ChatRow::size);
    chat.RegisterMember("tone", &ChatRow::tone);
  }
  if (auto cell = ctor.RegisterStruct<Cell>()) {
    cell.RegisterMember("thumb", &Cell::thumb);
    cell.RegisterMember("size", &Cell::size);
    cell.RegisterMember("video", &Cell::video);
    cell.RegisterMember("picked", &Cell::picked);
  }
  if (auto file = ctor.RegisterStruct<FileRow>()) {
    file.RegisterMember("name", &FileRow::name);
    file.RegisterMember("detail", &FileRow::detail);
    file.RegisterMember("size", &FileRow::size);
    file.RegisterMember("ext", &FileRow::ext);
    file.RegisterMember("picked", &FileRow::picked);
  }
  ctor.RegisterArray<std::vector<Segment>>();
  ctor.RegisterArray<std::vector<ChatRow>>();
  ctor.RegisterArray<std::vector<Cell>>();
  ctor.RegisterArray<std::vector<FileRow>>();

  ctor.Bind("storage_page", &page_binding_);
  ctor.Bind("storage_ready", &ready_);
  ctor.Bind("storage_total_label", &total_label_);
  ctor.Bind("storage_segments", &segments_);
  ctor.Bind("storage_manage_label", &manage_label_);
  ctor.Bind("storage_manage_segments", &manage_segments_);
  ctor.Bind("storage_large_visible", &large_visible_);
  ctor.Bind("storage_large_count_label", &large_count_label_);
  ctor.Bind("storage_large_size_label", &large_size_label_);
  ctor.Bind("storage_chats", &chats_);
  ctor.Bind("storage_media_empty", &media_empty_);
  ctor.Bind("storage_items_title", &items_title_);
  ctor.Bind("storage_tab", &tab_binding_);
  ctor.Bind("storage_tab_picture_label", &tab_picture_label_);
  ctor.Bind("storage_tab_video_label", &tab_video_label_);
  ctor.Bind("storage_tab_file_label", &tab_file_label_);
  ctor.Bind("storage_cells", &cells_);
  ctor.Bind("storage_files", &files_);
  ctor.Bind("storage_list_empty", &list_empty_);
  ctor.Bind("storage_has_more", &has_more_);
  ctor.Bind("storage_selecting", &selecting_);
  ctor.Bind("storage_busy", &busy_);
  ctor.Bind("storage_has_selection", &has_selection_);
  ctor.Bind("storage_selection_label", &selection_label_);

  ctor.BindEventCallback("storage_open_manage", DataEvent<>([this]() { OpenManage(); }));
  ctor.BindEventCallback("storage_open_large", DataEvent<>([this]() { OpenLarge(); }));
  ctor.BindEventCallback("storage_open_chat", DataEvent<int>([this](const int index) { OpenChat(index); }));
  ctor.BindEventCallback("storage_set_tab", DataEvent<std::string>([this](const std::string& kind) { SetTab(kind); }));
  ctor.BindEventCallback("storage_tap_cell", DataEvent<int>([this](const int index) { TapCell(index); }));
  ctor.BindEventCallback("storage_tap_file", DataEvent<int>([this](const int index) { TapFile(index); }));
  ctor.BindEventCallback("storage_load_more", DataEvent<>([this]() { LoadMore(); }));
  ctor.BindEventCallback("storage_toggle_select", DataEvent<>([this]() { ToggleSelect(); }));
  ctor.BindEventCallback("storage_back", DataEvent<>([this]() { Back(); }));
  ctor.BindEventCallback("storage_delete", DataEvent<>([this]() { DeleteSelected(); }));

  if (!provider_registered_) {
    provider_registered_ = true;
    // A long press on a cell or file row selects it (no menu): the provider only notes which one.
    ContextMenuHost::Instance().RegisterProvider(
        [this, token = lifetime_.token(), snapshot = lifetime_.Snapshot()](const ContextMenuRequest& request) {
          if (!pp::Lifetime::Alive(token, snapshot) || page_ != "items") {
            return std::vector<ContextMenuAction>{};
          }
          for (ui::Element* cur = request.target; cur != nullptr; cur = cur->GetParentNode()) {
            const char* attribute = cur->HasAttribute("storage-cell")   ? "storage-cell"
                                    : cur->HasAttribute("storage-file") ? "storage-file"
                                                                        : nullptr;
            if (attribute != nullptr) {
              const int index = cur->GetAttribute<int>(attribute, -1);
              // After the press is handled: the rows re-render.
              AppRuntime::PostUI(lifetime_.Bind([this, index]() { PickByLongPress(index); }));
              break;
            }
          }
          return std::vector<ContextMenuAction>{};
        });
  }
}

void StorageManagerController::Enter() {
  Reset();
  // Once: the hub reads its callback from a worker, so it is not swapped while it may be read.
  if (!on_changed_installed_ && commands_.storage.set_on_changed) {
    on_changed_installed_ = true;
    commands_.storage.set_on_changed(lifetime_.Bind([this]() { OnChanged(); }));
  }
  ready_ = false;
  DirtyAll();
  StartRecompute();
}

void StorageManagerController::Reset() {
  ClearBackStep(kStepSelect);
  ClearBackStep(kStepItems);
  ClearBackStep(kStepManage);
  LeaveItems();
  page_ = "main";
  gate_.Reset();
  ++refresh_seq_; // a count in flight is for a page that is gone
  DirtyAll();
}

void StorageManagerController::RestoreBackStack() {
  ClearBackStep(kStepSelect);
  ClearBackStep(kStepItems);
  ClearBackStep(kStepManage);
  if (page_ == "main") {
    return;
  }
  PushBackStep(kStepManage, [this]() {
    page_ = "main";
    DirtyAll();
  });
  if (page_ == "items") {
    PushBackStep(kStepItems, [this]() {
      LeaveItems();
      page_ = "manage";
      DirtyAll();
    });
    if (selecting_) {
      PushBackStep(kStepSelect, [this]() { ExitSelect(false); });
    }
  }
}

void StorageManagerController::OpenManage() {
  if (page_ != "main") {
    return;
  }
  page_ = "manage";
  PushBackStep(kStepManage, [this]() {
    page_ = "main";
    DirtyAll();
  });
  DirtyAll();
}

void StorageManagerController::OpenLarge() {
  if (page_ != "manage" || overview_.large.empty()) {
    return;
  }
  items_large_ = true;
  items_thread_.clear();
  items_title_ = Tr("settings.storage.manage.large").c_str();
  EnterItems();
}

void StorageManagerController::OpenChat(const int index) {
  if (page_ != "manage" || index < 0 || static_cast<size_t>(index) >= chat_ids_.size()) {
    return;
  }
  items_large_ = false;
  items_thread_ = chat_ids_[static_cast<size_t>(index)];
  items_title_ = chats_[static_cast<size_t>(index)].name;
  EnterItems();
}

void StorageManagerController::EnterItems() {
  page_ = "items";
  selecting_ = false;
  selected_.clear();
  releasable_.reset();
  BuildPageItems();
  // The first kind this page has, pictures when it has none.
  tab_ = "picture";
  const StorageKindTotals totals = StorageTotalsOf(page_all_);
  if (totals.pictures == 0) {
    tab_ = totals.videos > 0 ? "video" : totals.files > 0 ? "file" : "picture";
  }
  shown_ = kStoragePageSize;
  NewGeneration();
  BuildCells();
  PushBackStep(kStepItems, [this]() {
    LeaveItems();
    page_ = "manage";
    DirtyAll();
  });
  DirtyAll();
}

void StorageManagerController::LeaveItems() {
  if (selecting_) {
    ClearBackStep(kStepSelect);
  }
  selecting_ = false;
  selected_.clear();
  releasable_.reset();
  ++releasable_seq_;
  if (generation_ != 0 && commands_.storage.cancel_thumbnails) {
    commands_.storage.cancel_thumbnails(generation_);
  }
  ++generation_;
  requested_.clear();
  page_all_.clear();
  tab_items_.clear();
  cells_.clear();
  files_.clear();
  UpdateSelectionLabel();
}

void StorageManagerController::NewGeneration() {
  if (generation_ != 0 && commands_.storage.cancel_thumbnails) {
    commands_.storage.cancel_thumbnails(generation_);
  }
  ++generation_;
  requested_.clear();
}

void StorageManagerController::SetTab(const std::string& kind) {
  if (page_ != "items" || kind == tab_ || !StoredKindFromId(kind)) {
    return;
  }
  tab_ = kind;
  shown_ = kStoragePageSize;
  NewGeneration();
  BuildCells();
  DirtyAll();
}

void StorageManagerController::BuildPageItems() {
  page_all_.clear();
  if (items_large_) {
    page_all_ = overview_.large;
  } else {
    for (const StoredAttachmentItem& item : all_items_) {
      if (item.thread_id == items_thread_) {
        page_all_.push_back(item);
      }
    }
  }
  const StorageKindTotals totals = StorageTotalsOf(page_all_);
  tab_picture_label_ = (KindLabel("picture") + " " + std::to_string(totals.pictures)).c_str();
  tab_video_label_ = (KindLabel("video") + " " + std::to_string(totals.videos)).c_str();
  tab_file_label_ = (KindLabel("file") + " " + std::to_string(totals.files)).c_str();
}

void StorageManagerController::BuildCells() {
  const StoredKind kind = StoredKindFromId(tab_).value_or(StoredKind::Picture);
  tab_items_ = items_large_ ? StorageItemsOfKind(page_all_, kind) : StorageItemsOfChat(page_all_, items_thread_, kind);
  const size_t count = std::min(shown_, tab_items_.size());
  cells_.clear();
  files_.clear();
  const int64_t now = WallNowMs();
  for (size_t i = 0; i < count; ++i) {
    const StoredAttachmentItem& item = tab_items_[i];
    const std::string key = HashKey(item.content_hash);
    const bool picked = selected_.count(key) != 0;
    if (kind == StoredKind::File) {
      files_.push_back({.name = (item.filename.empty() ? Tr("settings.storage.manage.unnamed_file") : item.filename)
                                    .c_str(),
                        .detail = MessageTimeLabel(item.timestamp_ms, now).c_str(),
                        .size = FormatByteSize(item.bytes).c_str(),
                        .ext = FileTypeLabel(item).c_str(),
                        .picked = picked});
      continue;
    }
    std::string thumb = commands_.storage.thumbnail_path
                            ? commands_.storage.thumbnail_path(item.thread_id, item.content_hash)
                            : std::string();
    // Only the cells on the page get a thumbnail made; a video with no poster is not decrypted for one.
    if (thumb.empty() && commands_.storage.request_thumbnail && requested_.insert(key).second) {
      commands_.storage.request_thumbnail(item.thread_id, item.content_hash, item.mime, generation_);
    }
    cells_.push_back({.thumb = thumb.c_str(),
                      .size = FormatByteSize(item.bytes).c_str(),
                      .video = kind == StoredKind::Video,
                      .picked = picked});
  }
  list_empty_ = tab_items_.empty();
  has_more_ = tab_items_.size() > count;
}

void StorageManagerController::RefreshThumbnails() {
  if (page_ != "items" || !commands_.storage.thumbnail_path) {
    return;
  }
  bool changed = false;
  for (size_t i = 0; i < cells_.size() && i < tab_items_.size(); ++i) {
    if (!cells_[i].thumb.empty()) {
      continue;
    }
    const std::string thumb = commands_.storage.thumbnail_path(tab_items_[i].thread_id, tab_items_[i].content_hash);
    if (!thumb.empty()) {
      cells_[i].thumb = thumb.c_str();
      changed = true;
    }
  }
  if (changed) {
    DataModelHost::Instance().Dirty(kModel, "storage_cells");
  }
}

void StorageManagerController::LoadMore() {
  if (page_ != "items" || !has_more_) {
    return;
  }
  shown_ += kStoragePageSize;
  BuildCells();
  DirtyAll();
}

void StorageManagerController::TapCell(const int index) {
  if (busy_ || index < 0 || static_cast<size_t>(index) >= cells_.size()) {
    return;
  }
  if (selecting_) {
    TogglePicked(index);
    return;
  }
  OpenItem(tab_items_[static_cast<size_t>(index)]);
}

void StorageManagerController::TapFile(const int index) {
  if (busy_ || index < 0 || static_cast<size_t>(index) >= files_.size()) {
    return;
  }
  if (selecting_) {
    TogglePicked(index);
    return;
  }
  OpenItem(tab_items_[static_cast<size_t>(index)]);
}

void StorageManagerController::PickByLongPress(const int index) {
  if (page_ != "items" || busy_ || index < 0 || static_cast<size_t>(index) >= tab_items_.size()) {
    return;
  }
  if (!selecting_) {
    EnterSelect();
  }
  if (selected_.count(HashKey(tab_items_[static_cast<size_t>(index)].content_hash)) == 0) {
    TogglePicked(index);
  }
}

void StorageManagerController::TogglePicked(const int index) {
  const std::string key = HashKey(tab_items_[static_cast<size_t>(index)].content_hash);
  bool picked = false;
  if (selected_.erase(key) == 0) {
    selected_.insert(key);
    picked = true;
  }
  if (static_cast<size_t>(index) < cells_.size()) {
    cells_[static_cast<size_t>(index)].picked = picked;
  }
  if (static_cast<size_t>(index) < files_.size()) {
    files_[static_cast<size_t>(index)].picked = picked;
  }
  SelectionChanged();
  DirtyAll();
}

void StorageManagerController::SelectionChanged() {
  releasable_.reset();
  const uint64_t seq = ++releasable_seq_;
  UpdateSelectionLabel();
  if (selected_.empty() || !commands_.storage.releasable_bytes) {
    return;
  }
  commands_.storage.releasable_bytes(StorageDeleteRefs(all_items_, SelectedItems(), items_large_),
                                     lifetime_.Bind([this, seq](const uint64_t bytes) {
                                       if (seq != releasable_seq_) {
                                         return;
                                       }
                                       releasable_ = bytes;
                                       UpdateSelectionLabel();
                                       DataModelHost::Instance().Dirty(kModel, "storage_selection_label");
                                     }));
}

void StorageManagerController::UpdateSelectionLabel() {
  has_selection_ = !selected_.empty();
  if (releasable_) {
    selection_label_ =
        TrCount("settings.storage.manage.selected_frees", selected_.size(), {{"size", FormatByteSize(*releasable_)}})
            .c_str();
  } else {
    selection_label_ = TrCount("settings.storage.manage.selected", selected_.size()).c_str();
  }
}

std::vector<StoredAttachmentItem> StorageManagerController::SelectedItems() const {
  std::vector<StoredAttachmentItem> items;
  std::set<std::string> seen;
  for (const StoredAttachmentItem& item : page_all_) {
    const std::string key = HashKey(item.content_hash);
    if (selected_.count(key) != 0 && seen.insert(key).second) {
      items.push_back(item);
    }
  }
  return items;
}

void StorageManagerController::ToggleSelect() {
  if (page_ != "items" || busy_) {
    return;
  }
  if (selecting_) {
    ExitSelect(true);
  } else {
    EnterSelect();
  }
}

void StorageManagerController::EnterSelect() {
  selecting_ = true;
  selected_.clear();
  SelectionChanged();
  PushBackStep(kStepSelect, [this]() { ExitSelect(false); });
  DirtyAll();
}

void StorageManagerController::ExitSelect(const bool clear_back_step) {
  if (clear_back_step) {
    ClearBackStep(kStepSelect);
  }
  selecting_ = false;
  selected_.clear();
  for (Cell& cell : cells_) {
    cell.picked = false;
  }
  for (FileRow& file : files_) {
    file.picked = false;
  }
  SelectionChanged();
  DirtyAll();
}

void StorageManagerController::Back() {
  if (selecting_) {
    ExitSelect(true);
    return;
  }
  if (page_ == "items") {
    ClearBackStep(kStepItems);
    LeaveItems();
    page_ = "manage";
  } else if (page_ == "manage") {
    ClearBackStep(kStepManage);
    page_ = "main";
  }
  DirtyAll();
}

void StorageManagerController::OpenItem(const StoredAttachmentItem& item) {
  if (!commands_.storage.open_item) {
    return;
  }
  const std::string key = item.thread_id + "\n" + HashKey(item.content_hash);
  if (!opening_.insert(key).second) {
    return; // a second tap while the first is still making the file
  }
  if (item.bytes > kSlowOpenBytes) {
    UserFeedback::Ok(Tr("chat.attachment.opening"));
  }
  const bool is_video = StoredKindOfMime(item.mime) == StoredKind::Video;
  commands_.storage.open_item(item, lifetime_.Bind([this, key, is_video](std::string path, const bool needs_confirm) {
    opening_.erase(key);
    if (path.empty()) {
      UserFeedback::Fail(Tr("chat.attachment.not_ready"));
      return;
    }
    const auto open_file = [path, is_video]() {
      if (!PlatformOpenFile(path)) {
        UserFeedback::Fail(Tr(is_video ? "chat.attachment.video_unplayable" : "chat.attachment.open_failed"));
      }
    };
    if (needs_confirm && feedback_.show_confirm) {
      feedback_.show_confirm(
          Tr("chat.attachment.open_confirm_title"), Tr("chat.attachment.open_confirm_body"),
          [open_file](const bool ok) {
            if (ok) {
              open_file();
            }
          },
          {});
      return;
    }
    open_file();
  }));
}

void StorageManagerController::DeleteSelected() {
  if (page_ != "items" || busy_ || selected_.empty() || !commands_.storage.delete_local) {
    return;
  }
  const std::vector<StoredAttachmentItem> selected = SelectedItems();
  std::vector<StoredAttachmentRef> refs = StorageDeleteRefs(all_items_, selected, items_large_);
  const StorageDeleteFacts facts = StorageDeleteFactsOf(all_items_, selected, refs);
  const uint64_t selected_bytes = StorageSelectedBytes(selected);
  auto ask = [this, refs, facts, selected_bytes](const uint64_t releasable) {
    if (!feedback_.show_confirm) {
      return;
    }
    const std::string title =
        TrCount("settings.storage.manage.delete_title", facts.count, {{"size", FormatByteSize(releasable)}});
    std::string message = Tr("settings.storage.manage.delete_body");
    if (facts.only_here > 0) {
      message += " " + Tr("settings.storage.manage.delete_only_here", {{"count", std::to_string(facts.only_here)}});
    }
    if (releasable < selected_bytes && facts.shared_elsewhere > 0) {
      message +=
          " " + Tr("settings.storage.manage.delete_shared", {{"count", std::to_string(facts.shared_elsewhere)}});
    }
    feedback_.show_confirm(title, message, lifetime_.Bind([this, refs](const bool ok) {
                             if (ok) {
                               PerformDelete(refs);
                             }
                           }),
                           Tr("settings.storage.manage.delete"));
  };
  if (!commands_.storage.releasable_bytes) {
    ask(selected_bytes);
    return;
  }
  // Asked afresh: what is freed may have changed since the label was made.
  commands_.storage.releasable_bytes(refs, lifetime_.Bind(std::move(ask)));
}

void StorageManagerController::PerformDelete(std::vector<StoredAttachmentRef> refs) {
  if (busy_ || !commands_.storage.delete_local) {
    return;
  }
  busy_ = true; // the page greys and ignores taps until the delete is done
  DirtyAll();
  commands_.storage.delete_local(std::move(refs), lifetime_.Bind([this](const StoredAttachmentDeleteResult& /*r*/) {
    busy_ = false;
    if (selecting_) {
      ExitSelect(true);
    }
    StartRecompute(); // rebuilt from what is on the device now
    DirtyAll();
  }));
}

void StorageManagerController::OnChanged() {
  if (!visible_ || !visible_()) {
    return; // counted afresh when the section is entered
  }
  RefreshThumbnails();
  Follow(gate_.OnChange(NowMs()));
}

void StorageManagerController::Follow(const StorageRecomputeGate::Decision decision) {
  switch (decision.step) {
  case StorageRecomputeGate::Step::Nothing:
    return;
  case StorageRecomputeGate::Step::RunNow:
    StartRecompute();
    return;
  case StorageRecomputeGate::Step::RunLater:
    if (!commands_.storage.run_later) {
      StartRecompute();
      return;
    }
    commands_.storage.run_later(std::chrono::milliseconds(decision.delay_ms), lifetime_.Bind([this]() {
                                  if (visible_ && !visible_()) {
                                    gate_.Reset();
                                    return;
                                  }
                                  Follow(gate_.OnTimer(NowMs()));
                                }));
    return;
  }
}

void StorageManagerController::StartRecompute() {
  gate_.OnStarted(NowMs());
  const uint64_t seq = ++refresh_seq_;
  auto counts = std::make_shared<Counts>();
  const auto finish = [this, seq, counts]() {
    if (--counts->left > 0 || seq != refresh_seq_) {
      return;
    }
    ApplyCounts(std::move(*counts));
    Follow(gate_.OnFinished(NowMs()));
  };
  const StorageManagerPorts& ports = commands_.storage;
  if (ports.list_stored) {
    ports.list_stored(lifetime_.Bind([counts, finish](std::vector<StoredAttachmentItem> items) {
      counts->items = std::move(items);
      finish();
    }));
  } else {
    finish();
  }
  if (ports.message_store_bytes) {
    ports.message_store_bytes(lifetime_.Bind([counts, finish](const uint64_t bytes) {
      counts->message_store_bytes = bytes;
      finish();
    }));
  } else {
    finish();
  }
  if (ports.profile_bytes) {
    ports.profile_bytes(lifetime_.Bind([counts, finish](const uint64_t bytes) {
      counts->profile_bytes = bytes;
      finish();
    }));
  } else {
    finish();
  }
}

void StorageManagerController::ApplyCounts(Counts counts) {
  all_items_ = std::move(counts.items);
  overview_ = BuildStorageOverview(all_items_);
  usage_ = ComputeStorageUsage(counts.profile_bytes, overview_.totals, counts.message_store_bytes);
  names_ = commands_.storage.chat_names ? commands_.storage.chat_names() : std::vector<StorageChatName>{};
  ready_ = true;
  BuildOverviewRows();
  if (page_ == "items") {
    // Same page, fresh data: what is still here stays selected and the shown count stays (no scroll jump).
    BuildPageItems();
    std::set<std::string> present;
    for (const StoredAttachmentItem& item : page_all_) {
      present.insert(HashKey(item.content_hash));
    }
    const size_t before = selected_.size();
    for (auto it = selected_.begin(); it != selected_.end();) {
      it = present.count(*it) != 0 ? std::next(it) : selected_.erase(it);
    }
    BuildCells();
    if (selected_.size() != before || selecting_) {
      SelectionChanged();
    }
  }
  DirtyAll();
}

void StorageManagerController::BuildOverviewRows() {
  total_label_ = FormatByteSize(usage_.total).c_str();
  segments_ = SegmentRows(StorageBarSegments(usage_, /*with_chats=*/true));
  manage_label_ = FormatByteSize(overview_.totals.Bytes()).c_str();
  manage_segments_ = SegmentRows(StorageBarSegments(usage_, /*with_chats=*/false));
  large_visible_ = !overview_.large.empty();
  large_count_label_ = TrCount("settings.storage.manage.large_count", overview_.large.size()).c_str();
  large_size_label_ = FormatByteSize(overview_.large_bytes).c_str();
  media_empty_ = overview_.totals.Count() == 0;

  std::unordered_map<std::string, const StorageChatName*> by_id;
  for (const StorageChatName& name : names_) {
    by_id[name.thread_id] = &name;
  }
  chats_.clear();
  chat_ids_.clear();
  for (const StorageChatSummary& chat : overview_.chats) {
    const auto found = by_id.find(chat.thread_id);
    std::string name = found != by_id.end() ? found->second->name : std::string();
    const std::string seed = found != by_id.end() ? found->second->avatar_seed : chat.thread_id;
    if (name.empty()) {
      name = Tr("people_picker.unnamed");
    }
    const AvatarGlyph glyph = MakeAvatarGlyph(name, seed);
    chats_.push_back({.name = name.c_str(),
                      .letter = glyph.letter.c_str(),
                      .summary = CountsLine(chat.totals).c_str(),
                      .size = FormatByteSize(chat.totals.Bytes()).c_str(),
                      .tone = glyph.tone});
    chat_ids_.push_back(chat.thread_id);
    if (page_ == "items" && !items_large_ && chat.thread_id == items_thread_) {
      items_title_ = name.c_str();
    }
  }
}

void StorageManagerController::PushBackStep(const char* id, std::function<void()> commit) {
  if (!navigation_.push_local_back) {
    return;
  }
  navigation_.push_local_back(id, lifetime_.Bind([this, commit = std::move(commit)]() {
    commit();
    // A swipe back slid the whole page away; the page above stays, so the shell puts it back in place (attaching
    // the gesture again resets the slide) once this frame's DOM is updated.
    if (navigation_.refresh_dismiss_gestures) {
      AppRuntime::PostUI(lifetime_.Bind([this]() {
        if (navigation_.refresh_dismiss_gestures) {
          navigation_.refresh_dismiss_gestures();
        }
      }));
    }
  }));
}

void StorageManagerController::ClearBackStep(const char* id) {
  if (navigation_.clear_local_back) {
    navigation_.clear_local_back(id);
  }
}

void StorageManagerController::DirtyAll() {
  page_binding_ = page_.c_str();
  tab_binding_ = tab_.c_str();
  auto& host = DataModelHost::Instance();
  for (const char* key :
       {"storage_page", "storage_ready", "storage_total_label", "storage_segments", "storage_manage_label",
        "storage_manage_segments", "storage_large_visible", "storage_large_count_label", "storage_large_size_label",
        "storage_chats", "storage_media_empty", "storage_items_title", "storage_tab", "storage_tab_picture_label",
        "storage_tab_video_label", "storage_tab_file_label", "storage_cells", "storage_files", "storage_list_empty",
        "storage_has_more", "storage_selecting", "storage_busy", "storage_has_selection",
        "storage_selection_label"}) {
    host.Dirty(kModel, key);
  }
}

} // namespace pbr
