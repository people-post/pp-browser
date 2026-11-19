#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace pbr {

/**
 * Every received attachment has a way out of the app (DEV/PLAN-received-file-save-as.md): on a computer a click on a
 * file asks where to save it, and every attachment's right-click menu has "Save as…"; taps on pictures and videos (and
 * every tap on a phone) stay as they were. These are the decisions; the chat screen only wires the ports.
 */

/** Only the platform decides (Platform::IsDesktop): an iPad, even full screen, is a Phone. */
enum class AttachmentLayout { Desktop, Phone };

/** What an attachment is, from what its sender declared (nothing is decrypted to know it). */
enum class AttachmentExitKind {
  Picture,
  Video,
  /** A sound file (music, a recording sent as a file): not a voice message. */
  AudioFile,
  /** A voice message recorded in the app (drawn as a voice bubble): no way out is offered for it. */
  Voice,
  File,
};

/**
 * The kind by the rules the transcript draws with: a voice bubble first (DecideVoiceBubble, when `voice_bubbles` — this
 * device draws them), then a picture or video mime, then a sound file (an audio mime or a sound file name: an `.mp3`
 * sent as application/octet-stream is one), else a file.
 */
AttachmentExitKind KindOfAttachment(const std::string& mime, const std::string& filename, uint64_t declared_bytes,
                                    int voice_max_seconds, bool voice_bubbles);

/** An attachment row of the open chat as it was last drawn (UI thread): what a tap or a menu needs to decide. */
struct DrawnAttachment {
  AttachmentExitKind kind = AttachmentExitKind::File;
  /** Downloaded: its card offers it (not the download / retry button). */
  bool ready = false;
  std::string mime;
  /** The name a save dialog offers (SuggestedSaveName). */
  std::string suggested_name;
};

/**
 * The name a save dialog offers: the name the system viewer is handed (AttachmentOpenFilename, the same call the open
 * path makes: the sender's name cleaned, with the type's safe extension); for a type that has none (an archive) the
 * sender's name cleaned the same way, else "attachment". A sound file whose content is another container than its
 * name says gets one more suffix on the open path (it is sniffed from the decrypted view): not known before a path
 * is chosen, so the dialog's name lacks it then.
 */
std::string SuggestedSaveName(const std::string& sender_filename, const std::string& mime);

enum class AttachmentTap {
  /** Today's open (the system viewer / player, a phone's own handling). */
  Open,
  /** The save flow (AttachmentSaveFlow). */
  Save,
  /** A sound file this computer plays in the app: today's path for it, never a save dialog. */
  PlayInApp,
  /** Not downloaded (or failed): "Attachment isn't ready yet". */
  NotReady,
  /** A save is being chosen or written: nothing happens. */
  Ignore,
};

/**
 * A tap on an attachment card. Phone: always Open (as before). Desktop: pictures, videos and voice messages Open, a
 * sound file the app plays (`plays_in_app`) PlayInApp, whatever a save dialog is up or a save runs (they save
 * nothing); any other file is Ignored while a save is busy, else NotReady when not `ready`, else Save.
 */
AttachmentTap DecideAttachmentTap(AttachmentLayout layout, AttachmentExitKind kind, bool ready, bool plays_in_app,
                                  bool save_busy);

/** The way-out items of a message's long-press / right-click menu. */
struct AttachmentMenuEntries {
  /** "Save as…" (desktop). */
  bool save_as = false;
  /** "Share…" (the system share sheet: iOS / iPadOS, Android). */
  bool share = false;
};

/**
 * For every attachment but a voice message (nothing for a message that is no attachment): "Save as…" on the desktop,
 * "Share…" where the platform has a file share sheet (`can_share_file`, PlatformCanShareFile).
 */
AttachmentMenuEntries DecideMenuEntries(AttachmentLayout layout, bool can_share_file,
                                        std::optional<AttachmentExitKind> kind);

/** One save at a time: Idle → Choosing (the dialog is up) → Saving (the copy runs) → Idle. UI thread. */
class AttachmentSaveState {
public:
  enum class Phase { Idle, Choosing, Saving };
  /** Idle → Choosing for `message_id`; false (nothing changes) when a save is already busy. */
  bool TryBegin(const std::string& message_id);
  /** Choosing → Saving; false when `message_id` is not the one being chosen. */
  bool Chosen(const std::string& message_id);
  /** Back to Idle when `message_id` is the one in progress. */
  void Finish(const std::string& message_id);
  /** Choosing → Idle (the dialog never answered); a copy already running (Saving) is not touched. */
  void ReleaseChoosing();
  bool Busy() const { return phase_ != Phase::Idle; }
  Phase phase() const { return phase_; }
  /** Names the dialog being waited for: changes with every TryBegin, so an answer to a released one is told apart. */
  uint64_t token() const { return token_; }

private:
  Phase phase_ = Phase::Idle;
  std::string message_id_;
  uint64_t token_ = 0;
};

/** The save dialog's answer, as the flow sees it. */
struct SaveChoice {
  enum class Kind { Chosen, Cancelled, Unavailable };
  Kind kind = Kind::Cancelled;
  std::string path;
};

/** What the save flow needs from the app. */
struct AttachmentSavePorts {
  /** Shows the save dialog offering `suggested_name` (UI thread); `done` runs once, on any thread. */
  std::function<void(const std::string& suggested_name, std::function<void(SaveChoice)> done)> choose_path;
  std::function<void(std::function<void()>)> post_ui;
  std::function<void(std::function<void()>)> post_worker;
  /** Worker: the attachment's decrypted view file (made when missing); none when it cannot be made now (locked). */
  std::function<std::optional<std::string>(const std::string& thread_id, const std::string& message_id)> view_path;
  /** Worker: copies the view to the chosen path (CopyFileInChunks); false when it failed (the target is untouched). */
  std::function<bool(const std::string& from, const std::string& to)> copy;
  /** UI: puts the downloaded mark on the saved file (best effort). */
  std::function<void(const std::string& saved_path)> mark_downloaded;
  /** UI: the name the file manager shows for a folder (os::FolderDisplayName). */
  std::function<std::string(const std::string& folder)> folder_name;
  /** UI: the bottom pill, a result / a failure. */
  std::function<void(const std::string& text)> ok;
  std::function<void(const std::string& text)> fail;
  /** Milliseconds on a steady clock (a dialog's age); empty = the real clock. */
  std::function<int64_t()> now_ms;
};

/**
 * Desktop "Save as" of one attachment: dialog (nothing decrypted before a path is chosen) → "Saving…" → the view copied
 * on a worker → the downloaded mark on the saved file → "Saved to <folder>". Nothing is opened, and nothing is
 * deleted but a failed copy's temporary file. Every answer comes back to the UI thread before any state or pill
 * changes. The callbacks it hands out hold only a weak reference to the flow's state: when the flow (the chat
 * screen's member) is destroyed while a dialog or a copy is out, a late answer does nothing.
 */
class AttachmentSaveFlow {
public:
  enum class Start { Started, Ignored, NotReady };

  /** A dialog that has not answered for this long is taken as lost (portal hang, window closed with it up). */
  static constexpr int64_t kStaleDialogMs = 30000;

  explicit AttachmentSaveFlow(AttachmentSavePorts ports);

  /** A copy is running, or a dialog is up that is not stale: generic-file taps and "Save as…" wait. */
  bool Busy() const;
  /**
   * Drops a dialog still waiting (the open chat changed): its late answer is ignored and nothing is saved. A copy that
   * already runs goes on.
   */
  void ReleaseChoosing();
  /**
   * UI thread. Ignored (nothing happens) while another save is busy; NotReady (the pill says so) when the attachment
   * is not downloaded; else the dialog opens offering `suggested_name`.
   */
  Start Begin(const std::string& thread_id, const std::string& message_id, const std::string& suggested_name,
              bool ready);

private:
  /** The flow's state, shared with the callbacks it hands out: once the owner is gone they find nothing and do nothing. */
  struct Core;
  static void OnChoice(const std::shared_ptr<Core>& core, const std::string& thread_id, const std::string& message_id,
                       uint64_t token, const SaveChoice& choice);
  static void OnCopied(const std::shared_ptr<Core>& core, const std::string& message_id, const std::string& target,
                       bool copied);

  std::shared_ptr<Core> core_;
};

/** What the share flow needs from the app. */
struct AttachmentSharePorts {
  std::function<void(std::function<void()>)> post_ui;
  std::function<void(std::function<void()>)> post_worker;
  /** Worker: the attachment's decrypted view file (made when missing); none when it cannot be made now (locked). */
  std::function<std::optional<std::string>(const std::string& thread_id, const std::string& message_id)> view_path;
  /** UI: the system share sheet for `path`, pointing at (`x`, `y`) on an iPad; false when it could not be shown. */
  std::function<bool(const std::string& path, const std::string& mime, int x, int y)> share;
  /** UI: the bottom pill for a failure. */
  std::function<void(const std::string& text)> fail;
};

/**
 * "Share…" of one attachment (phones): its view is found (made on a worker when missing), then handed to the system
 * share sheet. A sheet that cannot be shown says "Can't share this file right now"; a share or a cancel says nothing.
 * The shared file is the view itself: nothing is deleted or touched afterwards. UI thread; the flow must outlive the
 * callbacks it hands out (it is the chat screen's member).
 */
class AttachmentShareFlow {
public:
  enum class Start { Started, Ignored, NotReady };

  explicit AttachmentShareFlow(AttachmentSharePorts ports) : ports_(std::move(ports)) {}

  /** The view is being found for a share: a second Share… waits (Ignored). */
  bool Busy() const { return busy_; }
  /** NotReady (the pill says so) when the attachment is not downloaded; (`x`, `y`) anchors the iPad popover. */
  Start Begin(const std::string& thread_id, const std::string& message_id, const std::string& mime, bool ready, int x,
              int y);

private:
  AttachmentSharePorts ports_;
  bool busy_ = false;
};

} // namespace pbr
