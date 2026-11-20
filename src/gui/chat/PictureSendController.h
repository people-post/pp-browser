#pragma once

#include "feature/presenters/PictureSendQueue.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/shell/ShellNavigationPorts.h"

#include <ui/base/Types.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {
class Context;
}

namespace pbr {

/** "12.4 MB": the size beside the "original" check (one decimal below ten MB, whole above). */
std::string FormatPickedSize(uint64_t bytes);

/** One cell of the page's thumbnail strip, as the page binds it. */
struct PictureSendCell {
  int index = 0;
  ui::String thumb_src;
  bool ready = false;
  bool current = false;
  bool too_large = false;
  /** The thumbnail covering the square cell without stretching: its size and offset in dp, with the unit. */
  ui::String iw = "0dp";
  ui::String ih = "0dp";
  ui::String ml = "0dp";
  ui::String mt = "0dp";
};

/** What the preview worker made for one picked file (empty paths: none could be made, or it was told to stop). */
struct PictureSendPreviewFiles {
  std::string preview_path;
  std::string thumb_path;
  /** The preview's pixel size (upright), so the page can fit it into the space it has without stretching it. */
  int width = 0;
  int height = 0;
};

/** What the page needs from the chat screen. Every port is called on the UI thread. */
struct PictureSendHost {
  /** The queue of the pictures on the page (null when there is none). The page never owns it. */
  std::function<PictureSendQueue*()> queue;
  /** The folder the page's own files may go into (empty: no files can be made, the page shows blanks). */
  std::function<std::string()> preview_root;
  /**
   * Makes the page's two pictures of `path` (long edge 1280 and a small one) into `dir`, on a worker thread, reading
   * a picture of at most `max_image_bytes` (the batch's current limit). Looks at `cancelled` before it writes and
   * after, and deletes what it wrote when told to stop.
   */
  std::function<PictureSendPreviewFiles(const std::string& path, PictureSendKind kind, const std::string& dir,
                                        uint64_t max_image_bytes, const std::function<bool()>& cancelled)>
      make_preview;
  /** Picture files the page showed are done with: their textures are released (two frames later, see QueueTextureRelease). */
  std::function<void(std::vector<std::string> srcs)> release_textures;
  /** After any call into the queue: the composer follows the attachment slot, the progress pill follows the queue. */
  std::function<void()> queue_changed;
  /** The page was left without sending: the pick that brought the pictures is over. */
  std::function<void()> left;
  /** The first preview picture is visible (the time from the picker finishing to here is a metric). */
  std::function<void()> first_preview_shown;
  /** The words: the number of pictures that will be sent, and the size of the whole choice. */
  std::function<std::string(size_t sendable)> send_label;
  std::function<std::string(uint64_t bytes)> size_label;
  /** Where background work and its answers run (default: AppRuntime's worker pool and UI mailbox). */
  std::function<void(std::function<void()>)> post_worker;
  std::function<void(std::function<void()>)> post_ui;
  /** Plays the picked video `path` in the system's player over the page (what a chat video opens in); false: it could not. */
  std::function<bool(const std::string& path)> play_video;
  /** Deletes a file / a folder (default: std::filesystem). */
  std::function<void(const std::string& path)> remove_path;
};

/**
 * The picture preview page (DEV/PLAN-picture-send-preview.md): a full-screen layer over the chat where the pictures
 * picked together are looked at, swiped through, removed, given a line of text and sent. It shows the items of the
 * chat's PictureSendQueue and drives that queue (Back, Send, remove, "original"); the queue decides everything about
 * sending. This class owns the page's own temporary files (the preview and the thumbnail of each picture, made one at
 * a time on a worker with a cancel flag) and the layer. UI thread.
 *
 * Files: a folder per open page, deleted only after the worker that may still write into it has stopped. A picture's
 * texture is bound only after its file is written, and released (two frames later) when its page is swiped away.
 */
class PictureSendController {
public:
  PictureSendController();
  ~PictureSendController();

  void BindShellNavigation(ShellNavigationPorts ports);
  void BindFlowCoordinator(FlowCoordinatorPorts ports);
  void SetHost(PictureSendHost host);
  bool RegisterModel(ui::Context* context);

  bool IsOpen() const { return layer_id_ >= 0; }
  /** The page has pictures but its first one is still being made (the page says "Preparing…"). */
  bool HasAnyPreview() const { return any_preview_; }

  /**
   * A picture was added to the queue (id `id`): opens the page when it is not open, queues its preview, and shows it.
   * False when the page cannot open (no shell): the caller backs the queue out.
   */
  bool OnItemAdded(uint64_t id);
  /** More pictures are still being copied out of the library (the page waits for them; Send waits too). */
  void SetArriving(bool arriving);
  /**
   * The lock, a sign-out, the chat deleted or teardown: the layer closes without a word to the queue (the caller ends
   * the queue itself), the worker is told to stop and the page's files go.
   */
  void CloseForExit();
  /** Everything the page held is dropped (teardown). */
  void Shutdown() { CloseForExit(); }

  // The page's own events (also what the tests tap).
  void OnBack();
  void OnSend();
  void OnRemove();
  void OnSelect(int index);
  void OnToggleOriginal();
  /** The current picture is a video: its play button opens the player (the page, its choices and text stay as they are). */
  void OnPlay();
  void OnPress(int x);
  void OnRelease(int x);

  /** The text field's current content (for the tests and the Back / Send hand-over). */
  std::string Text() const { return std::string(text_.c_str()); }
  /** How many times the page's bound state was marked changed (tests: an idle page must not do it every frame). */
  int DirtyCountForTest() const { return dirty_count_; }
  void SetTextForTest(const std::string& text) { text_ = text.c_str(); }
  int CurrentIndex() const { return current_; }
  const std::vector<PictureSendCell>& Cells() const { return cells_; }
  std::string CurrentSrc() const { return std::string(current_src_.c_str()); }
  bool CanSend() const { return can_send_; }
  /** The "original" check: off every time the page opens, set only by a tap, never stored anywhere. */
  bool OriginalChecked() const { return original_; }
  /** Every frame while the page is open: the picture is fitted again when the space for it changed (the keyboard). */
  void Update();
  /** A preview worker is running (the folder of a stopped page is deleted when it reports). */
  bool WorkerRunning() const { return worker_running_; }

private:
  /** The layer was closed by anyone (back key, swipe, a tapped notification): the page is left. */
  void Leave();
  void Open();
  /** Ends the page's own business: stop flag, queued jobs dropped, textures released, files deleted (or deferred). */
  void Stop();
  void Refresh();
  void PumpJobs();
  void RemovePath(const std::string& path);
  void ReleaseTextures(std::vector<std::string> srcs);
  void ReleaseAllTextures();
  void NoteTexture(const std::string& path);
  void Dirty();
  void FitCurrent();
  PictureSendQueue* Queue() const;
  void RemoveDir(const std::string& dir);
  static void OnPreviewDone(const std::weak_ptr<int>& alive, PictureSendController* self, uint64_t session, uint64_t id,
                            PictureSendPreviewFiles files, const std::shared_ptr<std::atomic<bool>>& stop,
                            const std::shared_ptr<std::atomic<bool>>& cancel, const std::string& dir);
  std::shared_ptr<std::atomic<bool>> CancelFlag(uint64_t id);

  ShellNavigationPorts navigation_;
  FlowCoordinatorPorts flow_;
  PictureSendHost host_;
  ui::Context* context_ = nullptr;
  int layer_id_ = -1;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  // The page's session: one per opening.
  uint64_t session_ = 0;
  std::string dir_;
  std::shared_ptr<std::atomic<bool>> stop_ = std::make_shared<std::atomic<bool>>(false);
  std::deque<uint64_t> jobs_;
  std::vector<std::pair<uint64_t, std::shared_ptr<std::atomic<bool>>>> item_cancel_;
  bool worker_running_ = false;
  uint64_t running_session_ = 0;
  bool arriving_ = false;
  bool any_preview_ = false;
  bool first_shown_ = false;
  int current_ = 0;
  int press_x_ = 0;
  bool press_pending_ = false;
  int dirty_count_ = 0;
  uint64_t running_id_ = 0;
  std::string shown_src_;
  std::vector<std::string> textures_;

  // What the page binds.
  ui::String title_;
  std::vector<PictureSendCell> cells_;
  bool show_strip_ = false;
  ui::String current_src_;
  ui::String image_w_ = "0dp";
  ui::String image_h_ = "0dp";
  float stage_w_ = 0;
  float stage_h_ = 0;
  std::vector<std::pair<uint64_t, std::pair<int, int>>> dims_;
  uint64_t current_id_ = 0;
  bool current_ready_ = false;
  bool current_video_ = false;
  bool current_too_large_ = false;
  bool has_items_ = false;
  ui::String text_;
  bool original_ = false;
  ui::String size_label_;
  ui::String send_label_;
  bool can_send_ = false;
  bool current_preparing_ = false;
  std::vector<uint64_t> failed_;
};

} // namespace pbr
