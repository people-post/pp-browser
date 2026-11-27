#include "gui/chat/PictureSendController.h"

#include "common/runtime/AppRuntime.h"
#include "gui/chat/TranscriptRml.h"
#include "gui/shell/DataModelHost.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/ui/DataEvent.h"

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace pbr {

std::string FormatPickedSize(const uint64_t bytes) {
  const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
  char text[32];
  std::snprintf(text, sizeof(text), mb < 10.0 ? "%.1f MB" : "%.0f MB", mb);
  return text;
}

namespace {

/** A sideways move of a finger / pointer this long (dp) turns the page. */
constexpr int kSwipeDp = 48;
/** The thumbnail cell's side; the stylesheet's .ps-cell is this size. */
constexpr int kCellDp = 64;

void RemoveWhole(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

} // namespace

PictureSendController::PictureSendController() = default;

PictureSendController::~PictureSendController() {
  if (stop_) {
    stop_->store(true); // a worker still running writes nothing more (its answer finds no controller)
  }
}

void PictureSendController::BindShellNavigation(ShellNavigationPorts ports) {
  navigation_ = std::move(ports);
}

void PictureSendController::BindFlowCoordinator(FlowCoordinatorPorts ports) {
  flow_ = std::move(ports);
}

void PictureSendController::SetHost(PictureSendHost host) {
  host_ = std::move(host);
}

bool PictureSendController::RegisterModel(ui::Context* context) {
  context_ = context;
  if (!context) {
    return false;
  }
  return DataModelHost::Instance().Register(context, "picture_send", [this](ui::DataModelConstructor& ctor) {
    if (auto cell = ctor.RegisterStruct<PictureSendCell>()) {
      cell.RegisterMember("index", &PictureSendCell::index);
      cell.RegisterMember("thumb_src", &PictureSendCell::thumb_src);
      cell.RegisterMember("ready", &PictureSendCell::ready);
      cell.RegisterMember("current", &PictureSendCell::current);
      cell.RegisterMember("too_large", &PictureSendCell::too_large);
      cell.RegisterMember("is_file", &PictureSendCell::is_file);
      cell.RegisterMember("name", &PictureSendCell::name);
      cell.RegisterMember("size_label", &PictureSendCell::size_label);
      cell.RegisterMember("ext", &PictureSendCell::ext);
      cell.RegisterMember("iw", &PictureSendCell::iw);
      cell.RegisterMember("ih", &PictureSendCell::ih);
      cell.RegisterMember("ml", &PictureSendCell::ml);
      cell.RegisterMember("mt", &PictureSendCell::mt);
    }
    ctor.RegisterArray<std::vector<PictureSendCell>>();
    ctor.Bind("ps_title", &title_);
    ctor.Bind("ps_cells", &cells_);
    ctor.Bind("ps_show_strip", &show_strip_);
    ctor.Bind("ps_current_src", &current_src_);
    ctor.Bind("ps_current_ready", &current_ready_);
    ctor.Bind("ps_image_w", &image_w_);
    ctor.Bind("ps_image_h", &image_h_);
    ctor.Bind("ps_current_preparing", &current_preparing_);
    ctor.Bind("ps_current_video", &current_video_);
    ctor.Bind("ps_current_too_large", &current_too_large_);
    ctor.Bind("ps_current_is_file", &current_is_file_);
    ctor.Bind("ps_current_name", &current_name_);
    ctor.Bind("ps_current_size", &current_size_);
    ctor.Bind("ps_current_ext", &current_ext_);
    ctor.Bind("ps_show_original", &show_original_);
    ctor.Bind("ps_desktop", &desktop_);
    ctor.Bind("ps_recipient", &recipient_);
    ctor.Bind("ps_has_items", &has_items_);
    ctor.Bind("ps_text", &text_);
    ctor.Bind("ps_original", &original_);
    ctor.Bind("ps_size_label", &size_label_);
    ctor.Bind("ps_send_label", &send_label_);
    ctor.Bind("ps_can_send", &can_send_);
    ctor.BindEventCallback("ps_back", DataEvent([this] { OnBack(); }));
    ctor.BindEventCallback("ps_send", DataEvent([this] { OnSend(); }));
    ctor.BindEventCallback("ps_remove", DataEvent([this] { OnRemove(); }));
    ctor.BindEventCallback("ps_select", DataEvent<int>([this](const int index) { OnSelect(index); }));
    ctor.BindEventCallback("ps_toggle_original", DataEvent([this] { OnToggleOriginal(); }));
    ctor.BindEventCallback("ps_play", DataEvent([this] { OnPlay(); }));
    ctor.BindEventCallback("ps_press",
                           DataEventAt([this](ui::Event& ev) { OnPress(ev.GetParameter<int>("mouse_x", 0)); }));
    ctor.BindEventCallback("ps_release",
                           DataEventAt([this](ui::Event& ev) { OnRelease(ev.GetParameter<int>("mouse_x", 0)); }));
  });
}

PictureSendQueue* PictureSendController::Queue() const {
  return host_.queue ? host_.queue() : nullptr;
}

void PictureSendController::Dirty() {
  ++dirty_count_;
  if (context_) {
    DataModelHost::Instance().DirtyAll("picture_send");
  }
}

void PictureSendController::RemovePath(const std::string& path) {
  if (host_.remove_path) {
    host_.remove_path(path);
  } else {
    RemoveWhole(path);
  }
}

void PictureSendController::RemoveDir(const std::string& dir) {
  if (dir.empty()) {
    return;
  }
  RemovePath(dir);
  // The folder that held the page folders goes too once nothing is in it (a plain remove: never a folder with files).
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(dir).parent_path(), ec);
}

void PictureSendController::NoteTexture(const std::string& path) {
  if (!path.empty() && std::find(textures_.begin(), textures_.end(), path) == textures_.end()) {
    textures_.push_back(path);
  }
}

void PictureSendController::ReleaseTextures(std::vector<std::string> srcs) {
  // The one way a texture is given back: what is released leaves the held list, so no file is released twice.
  std::vector<std::string> out;
  for (std::string& src : srcs) {
    const auto it = std::find(textures_.begin(), textures_.end(), src);
    if (it != textures_.end()) {
      textures_.erase(it);
      out.push_back(std::move(src));
    }
  }
  if (!out.empty() && host_.release_textures) {
    host_.release_textures(std::move(out));
  }
}

void PictureSendController::ReleaseAllTextures() {
  ReleaseTextures(std::vector<std::string>(textures_));
}

std::shared_ptr<std::atomic<bool>> PictureSendController::CancelFlag(const uint64_t id) {
  for (auto& entry : item_cancel_) {
    if (entry.first == id) {
      return entry.second;
    }
  }
  item_cancel_.emplace_back(id, std::make_shared<std::atomic<bool>>(false));
  return item_cancel_.back().second;
}

void PictureSendController::Open() {
  if (IsOpen() || !navigation_.push_layer) {
    return;
  }
  ++session_;
  stop_ = std::make_shared<std::atomic<bool>>(false);
  const std::string root = host_.preview_root ? host_.preview_root() : std::string();
  dir_ = root.empty() ? std::string() : (std::filesystem::path(root) / std::to_string(session_)).string();
  jobs_.clear();
  item_cancel_.clear();
  failed_.clear();
  dims_.clear();
  textures_.clear();
  arriving_ = false;
  any_preview_ = false;
  first_shown_ = false;
  current_ = 0;
  shown_src_.clear();
  press_pending_ = false;
  original_ = false;
  text_ = "";
  PaneSpec spec;
  spec.key = "picture_send";
  spec.rml_path = "picture_send";
  spec.role = PaneRole::Transient;
  spec.full_screen = true;
  layer_id_ = navigation_.push_layer(spec);
  if (!IsOpen()) {
    return;
  }
  if (flow_.begin_modal) {
    // The back key, a swipe and any close of the layer (a tapped notification clears them all) are all "Back".
    flow_.begin_modal(
        layer_id_, []() { return false; },
        [this]() {
          layer_id_ = -1;
          Leave();
        });
  }
}

bool PictureSendController::OnItemAdded(const uint64_t id) {
  Open();
  if (!IsOpen()) {
    return false;
  }
  if (!dir_.empty()) {
    CancelFlag(id);
    jobs_.push_back(id);
    PumpJobs();
  }
  Refresh();
  return true;
}

void PictureSendController::SetArriving(const bool arriving) {
  if (arriving_ == arriving) {
    return;
  }
  arriving_ = arriving;
  if (!IsOpen()) {
    return;
  }
  PictureSendQueue* queue = Queue();
  if (!arriving && queue && queue->Items().empty()) {
    OnBack(); // everything on the page was removed while the rest was still coming, and nothing more came
    return;
  }
  Refresh();
}

void PictureSendController::PumpJobs() {
  if (worker_running_ || stop_->load()) {
    return;
  }
  PictureSendQueue* queue = Queue();
  while (queue && !jobs_.empty()) {
    const uint64_t id = jobs_.front();
    jobs_.pop_front();
    const auto& items = queue->Items();
    const auto it =
        std::find_if(items.begin(), items.end(), [id](const PictureSendItem& item) { return item.id == id; });
    if (it == items.end()) {
      continue; // removed before its turn
    }
    if (it->too_large || it->kind == PictureSendKind::File) {
      continue; // over the limit for how it would go (the cell shows its label), or a file: nothing to make a preview of
    }
    worker_running_ = true;
    running_id_ = id;
    running_session_ = session_;
    const auto stop = stop_;
    const auto cancel = CancelFlag(id);
    const std::string dir = dir_;
    const std::string path = it->path;
    const PictureSendKind kind = it->kind;
    const auto make = host_.make_preview;
    const uint64_t max_image_bytes = queue->Limits().max_original_image_bytes;
    const auto post_ui = host_.post_ui;
    const uint64_t session = session_;
    const std::weak_ptr<int> alive = alive_;
    auto work = [make, post_ui, stop, cancel, dir, path, kind, max_image_bytes, session, id, alive, this]() {
      PictureSendPreviewFiles files;
      if (make && !stop->load() && !cancel->load()) {
        files = make(path, kind, dir, max_image_bytes, [stop, cancel]() { return stop->load() || cancel->load(); });
      }
      auto reply = [alive, this, session, id, files, stop, cancel, dir]() {
        OnPreviewDone(alive, this, session, id, files, stop, cancel, dir);
      };
      if (post_ui) {
        post_ui(std::move(reply));
      } else {
        pp::AppRuntime::PostUI(std::move(reply));
      }
    };
    if (host_.post_worker) {
      host_.post_worker(std::move(work));
    } else {
      pp::AppRuntime::PostWorkerNormal(std::move(work));
    }
    return;
  }
}

void PictureSendController::OnPreviewDone(const std::weak_ptr<int>& alive, PictureSendController* self,
                                          const uint64_t /*session*/, const uint64_t id, PictureSendPreviewFiles files,
                                          const std::shared_ptr<std::atomic<bool>>& stop,
                                          const std::shared_ptr<std::atomic<bool>>& cancel, const std::string& dir) {
  if (!alive.lock()) {
    return;
  }
  self->worker_running_ = false;
  self->running_id_ = 0;
  if (stop->load()) {
    // The page this was for is over: whatever was written goes, and the page folder with it now that nobody writes.
    self->RemoveDir(dir);
    self->PumpJobs();
    return;
  }
  PictureSendQueue* queue = self->Queue();
  if (files.preview_path.empty()) {
    if (!cancel->load()) {
      self->failed_.push_back(id); // no picture to show for this one: it can still be sent
    }
  } else if (queue && queue->SetPreview(id, files.preview_path, files.thumb_path)) {
    self->NoteTexture(files.thumb_path); // the large picture is noted when it is shown (Refresh)
    self->dims_.push_back({id, {files.width, files.height}});
    self->any_preview_ = true;
  } else {
    // Removed meanwhile (or sending began): the files are nobody's.
    for (const std::string& file : {files.preview_path, files.thumb_path}) {
      if (!file.empty()) {
        self->RemovePath(file);
      }
    }
  }
  self->Refresh();
  self->PumpJobs();
}

void PictureSendController::Refresh() {
  PictureSendQueue* queue = Queue();
  if (!queue || !IsOpen()) {
    return;
  }
  const auto& items = queue->Items();
  const int count = static_cast<int>(items.size());
  current_ = count == 0 ? 0 : std::clamp(current_, 0, count - 1);
  cells_.clear();
  for (int i = 0; i < count; ++i) {
    const PictureSendItem& item = items[static_cast<size_t>(i)];
    PictureSendCell cell;
    cell.index = i;
    cell.ready = !item.thumb_path.empty();
    cell.thumb_src = item.thumb_path.c_str();
    cell.current = i == current_;
    cell.too_large = item.too_large;
    if (item.kind == PictureSendKind::File) {
      cell.is_file = true;
      cell.name = item.display_name.c_str();
      cell.size_label = FormatAttachmentByteSize(item.bytes).c_str();
      cell.ext = FileTypeTileLabel(item.display_name).c_str();
    }
    for (const auto& entry : dims_) {
      if (entry.first == item.id && entry.second.first > 0 && entry.second.second > 0) {
        // Covers the square cell, centred: the longer side is cut, nothing is stretched.
        const float w = static_cast<float>(entry.second.first);
        const float h = static_cast<float>(entry.second.second);
        const float scale = static_cast<float>(kCellDp) / std::min(w, h);
        const int iw = static_cast<int>(w * scale);
        const int ih = static_cast<int>(h * scale);
        const auto dp = [](const int value) { return std::to_string(value) + "dp"; };
        cell.iw = dp(iw).c_str();
        cell.ih = dp(ih).c_str();
        cell.ml = dp(-(iw - kCellDp) / 2).c_str();
        cell.mt = dp(-(ih - kCellDp) / 2).c_str();
      }
    }
    cells_.push_back(std::move(cell));
  }
  has_items_ = count > 0;
  show_strip_ = count > 1;
  title_ = count == 0 ? "" : (std::to_string(current_ + 1) + " / " + std::to_string(count)).c_str();
  const bool any_file = std::any_of(items.begin(), items.end(),
                                    [](const PictureSendItem& item) { return item.kind == PictureSendKind::File; });
  if (any_file && host_.items_title) {
    title_ = host_.items_title(static_cast<size_t>(count)).c_str();
  }
  show_original_ = std::any_of(items.begin(), items.end(),
                               [](const PictureSendItem& item) { return item.kind == PictureSendKind::Picture; });
  desktop_ = host_.is_desktop && host_.is_desktop();
  recipient_ = host_.recipient ? host_.recipient().c_str() : "";

  std::string src;
  current_video_ = false;
  current_too_large_ = false;
  current_preparing_ = false;
  current_is_file_ = false;
  current_name_ = "";
  current_size_ = "";
  current_ext_ = "";
  if (count > 0) {
    const PictureSendItem& item = items[static_cast<size_t>(current_)];
    src = item.preview_path;
    current_id_ = item.id;
    current_video_ = item.kind == PictureSendKind::Video;
    current_too_large_ = item.too_large;
    if (item.kind == PictureSendKind::File) {
      current_is_file_ = true;
      current_name_ = item.display_name.c_str();
      current_size_ = FormatAttachmentByteSize(item.bytes).c_str();
      current_ext_ = FileTypeTileLabel(item.display_name).c_str();
    }
    const bool failed = std::find(failed_.begin(), failed_.end(), item.id) != failed_.end();
    current_preparing_ = src.empty() && !failed && !item.too_large && !current_is_file_;
  } else {
    current_preparing_ = arriving_;
  }
  if (src != shown_src_) {
    // The picture swiped away gives its texture back (two frames later); only the current one is held large.
    if (!shown_src_.empty()) {
      ReleaseTextures({shown_src_});
    }
    shown_src_ = src;
    NoteTexture(src); // held large from now (a swipe back loads it again, so it is held again)
  }
  current_ready_ = !src.empty();
  current_src_ = src.c_str(); // bound only now that the file exists
  FitCurrent();
  if (current_ready_ && !first_shown_) {
    first_shown_ = true;
    if (host_.first_preview_shown) {
      host_.first_preview_shown();
    }
  }
  const size_t sendable = queue->SendableCount();
  can_send_ = sendable > 0;
  send_label_ = host_.send_label ? host_.send_label(sendable).c_str() : "";
  size_label_ = original_ && host_.size_label ? host_.size_label(queue->SelectedBytes()).c_str() : "";
  Dirty();
}

void PictureSendController::FitCurrent() {
  // The picture's own proportions inside the space the stage has (the engine has no "contain" fit for an <img>).
  float box_w = stage_w_;
  float box_h = stage_h_;
  if (context_) {
    if (ui::ElementDocument* doc = context_->GetDocument(0)) {
      if (ui::Element* stage = doc->GetElementById("ps-stage")) {
        const float dpr = std::max(context_->GetDensityIndependentPixelRatio(), 0.5f);
        const ui::Vector2f size = stage->GetBox().GetSize(ui::BoxArea::Content) / dpr; // boxes are in pixels
        stage_w_ = size.x; // the observed size is kept even when collapsed, so Update() settles on it
        stage_h_ = size.y;
        if (size.x > 1.f && size.y > 1.f) {
          box_w = size.x;
          box_h = size.y;
        }
      }
    }
    if (box_w <= 1.f || box_h <= 1.f) {
      box_w = box_h = std::min(
          static_cast<float>(context_->GetDimensions().x) / std::max(context_->GetDensityIndependentPixelRatio(), 0.5f),
          600.f); // before the first layout
    }
  }
  int w = 0;
  int h = 0;
  for (const auto& entry : dims_) {
    if (entry.first == current_id_) {
      w = entry.second.first;
      h = entry.second.second;
    }
  }
  if (w <= 0 || h <= 0 || box_w <= 1.f || box_h <= 1.f) {
    image_w_ = "0dp";
    image_h_ = "0dp";
    return;
  }
  const float scale = std::min(box_w / static_cast<float>(w), box_h / static_cast<float>(h));
  image_w_ = (std::to_string(static_cast<int>(static_cast<float>(w) * scale)) + "dp").c_str();
  image_h_ = (std::to_string(static_cast<int>(static_cast<float>(h) * scale)) + "dp").c_str();
}

void PictureSendController::Update() {
  if (!IsOpen() || !context_) {
    return;
  }
  ui::ElementDocument* doc = context_->GetDocument(0);
  ui::Element* stage = doc ? doc->GetElementById("ps-stage") : nullptr;
  if (!stage) {
    return;
  }
  const ui::Vector2f size =
      stage->GetBox().GetSize(ui::BoxArea::Content) / std::max(context_->GetDensityIndependentPixelRatio(), 0.5f);
  if (std::abs(size.x - stage_w_) > 0.5f || std::abs(size.y - stage_h_) > 0.5f) {
    FitCurrent();
    Dirty();
  }
}

void PictureSendController::OnSelect(const int index) {
  PictureSendQueue* queue = Queue();
  if (!queue || !IsOpen() || index < 0 || index >= static_cast<int>(queue->Items().size()) || index == current_) {
    return;
  }
  current_ = index;
  Refresh();
}

void PictureSendController::OnPress(const int x) {
  press_x_ = x;
  press_pending_ = true;
}

void PictureSendController::OnRelease(const int x) {
  if (!press_pending_) {
    return; // a release whose press was not on the stage (a drag that began elsewhere) is not a swipe
  }
  press_pending_ = false;
  const int moved = x - press_x_;
  if (moved <= -kSwipeDp) {
    OnSelect(current_ + 1);
  } else if (moved >= kSwipeDp) {
    OnSelect(current_ - 1);
  }
}

void PictureSendController::OnToggleOriginal() {
  PictureSendQueue* queue = Queue();
  if (!queue || !IsOpen() || queue->Started()) {
    return;
  }
  original_ = !original_;
  queue->SetOriginal(original_); // the over-limit marks are decided again for the way they would go now
  // An item that is no longer over the limit and has no preview (none was made while it was) gets its turn now.
  for (const PictureSendItem& item : queue->Items()) {
    const bool queued = std::find(jobs_.begin(), jobs_.end(), item.id) != jobs_.end();
    const bool failed = std::find(failed_.begin(), failed_.end(), item.id) != failed_.end();
    if (!item.too_large && item.kind != PictureSendKind::File && item.preview_path.empty() && !queued && !failed &&
        item.id != running_id_ && !dir_.empty()) {
      jobs_.push_back(item.id);
    }
  }
  PumpJobs();
  Refresh();
}

void PictureSendController::OnPlay() {
  PictureSendQueue* queue = Queue();
  if (!queue || !IsOpen() || queue->Started() || !host_.play_video) {
    return;
  }
  const auto& items = queue->Items();
  if (current_ < 0 || current_ >= static_cast<int>(items.size())) {
    return;
  }
  const PictureSendItem& item = items[static_cast<size_t>(current_)];
  if (item.kind == PictureSendKind::Video && !item.path.empty()) {
    host_.play_video(item.path); // the picked copy itself: the player is the system's, nothing here decodes it
  }
}

void PictureSendController::OnRemove() {
  PictureSendQueue* queue = Queue();
  if (!queue || !IsOpen() || queue->Started()) {
    return;
  }
  const auto& items = queue->Items();
  if (current_ < 0 || current_ >= static_cast<int>(items.size())) {
    return;
  }
  const PictureSendItem& item = items[static_cast<size_t>(current_)];
  const uint64_t id = item.id;
  std::vector<std::string> gone;
  for (const std::string& file : {item.preview_path, item.thumb_path}) {
    if (!file.empty()) {
      gone.push_back(file);
    }
  }
  CancelFlag(id)->store(true); // a preview being made for it stops; what it writes is deleted when it reports
  if (!queue->Remove(id)) {
    return;
  }
  if (!gone.empty()) {
    if (std::find(gone.begin(), gone.end(), shown_src_) != gone.end()) {
      shown_src_.clear(); // released here: Refresh must not release it again
    }
    ReleaseTextures(std::move(gone));
  }
  if (queue->Items().empty() && !arriving_) {
    OnBack(); // nothing left: back to the chat
    return;
  }
  Refresh();
  if (host_.queue_changed) {
    host_.queue_changed();
  }
}

void PictureSendController::OnBack() {
  if (!IsOpen()) {
    return;
  }
  const int layer = layer_id_;
  layer_id_ = -1;
  if (flow_.end_modal) {
    flow_.end_modal();
  }
  if (navigation_.close_layer) {
    navigation_.close_layer(layer);
  }
  Leave();
}

void PictureSendController::Leave() {
  // Nothing is sent. What was typed goes back into the chat the pictures were picked in (the queue does that).
  if (PictureSendQueue* queue = Queue()) {
    queue->SetText(Text());
    queue->Back();
  }
  Stop();
  if (host_.left) {
    host_.left();
  }
  if (host_.queue_changed) {
    host_.queue_changed();
  }
}

void PictureSendController::OnSend() {
  PictureSendQueue* queue = Queue();
  if (!queue || !IsOpen() || queue->Started() || queue->SendableCount() == 0) {
    return;
  }
  if (host_.may_send && !host_.may_send()) {
    return; // the host said why (B236: a held chat); the page stays open with its pictures
  }
  queue->SetText(Text());
  if (!queue->Start()) {
    return;
  }
  // Sending began: the queue deleted the page's files; the layer goes and the chat shows the pictures arriving.
  const int layer = layer_id_;
  layer_id_ = -1;
  if (flow_.end_modal) {
    flow_.end_modal();
  }
  if (navigation_.close_layer) {
    navigation_.close_layer(layer);
  }
  Stop();
  if (host_.queue_changed) {
    host_.queue_changed();
  }
}

void PictureSendController::CloseForExit() {
  // Called every frame while the vault is locked: acts once (an open page or something left to clear), then not at
  // all, so the idle chat is not dirtied each frame.
  if (!IsOpen() && dir_.empty() && textures_.empty() && jobs_.empty() && item_cancel_.empty() && !arriving_ &&
      !has_items_) {
    return;
  }
  if (IsOpen()) {
    const int layer = layer_id_;
    layer_id_ = -1;
    if (flow_.end_modal) {
      flow_.end_modal();
    }
    if (navigation_.close_layer) {
      navigation_.close_layer(layer);
    }
  }
  Stop();
}

void PictureSendController::Stop() {
  stop_->store(true);
  jobs_.clear();
  for (auto& entry : item_cancel_) {
    entry.second->store(true);
  }
  item_cancel_.clear();
  ReleaseAllTextures();
  // The folder goes at once when nothing can still write into it; otherwise the worker's report deletes it.
  const bool writer_running = worker_running_ && running_session_ == session_;
  if (!writer_running) {
    RemoveDir(dir_);
  }
  dir_.clear();
  shown_src_.clear();
  arriving_ = false;
  any_preview_ = false;
  cells_.clear();
  has_items_ = false;
  show_strip_ = false;
  current_ready_ = false;
  current_preparing_ = false;
  current_src_ = "";
  can_send_ = false;
  title_ = "";
  current_is_file_ = false;
  show_original_ = true;
  Dirty();
}

} // namespace pbr
