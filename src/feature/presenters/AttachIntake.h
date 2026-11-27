#pragma once

#include "feature/presenters/PictureSendQueue.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbr {

/**
 * The one way files get into a chat's send preview on a computer (DEV/PLAN-desktop-drop-paste.md, A): the "+" menu's
 * pickers now, a drop and a paste later. Everything here is pure (no GUI, no thread of its own): the chat screen wires
 * the ports and shows what is asked.
 */

enum class AttachSource { Picker, Drop, Paste };

/** One file offered to the preview. `owned`: a file this app made for it (a pasted picture): deleted when it is not used. */
struct AttachCandidate {
  std::string path;
  bool owned = false;
};

/** What decides whether files may come in now; the default is "all clear". */
struct AttachState {
  /** Unlocked and messaging up. */
  bool ready = true;
  bool chat_open = true;
  /** The open composer is the AI's. */
  bool ai_composer = false;
  bool editing_message = false;
  bool input_disabled = false;
  /** The chat holds its sends (RefuseSendWhileHeld). */
  bool send_held = false;
  /** The send-preview page is already open. */
  bool preview_open = false;
  /** Another layer, a dialog, a PIN gate or a call's full-screen page is above the chat. */
  bool layer_above = false;
  /** A pasted picture is being written to disk (step D1; always false until then). */
  bool paste_in_flight = false;
  /** An attachment is being prepared or sent (a forward's upload counts). */
  bool attachment_busy = false;
};

/** Why files are not taken now, in the order the checks run (the first that holds). */
enum class AttachRefusal {
  None,
  NotReady,
  NoChat,
  AiComposer,
  Editing,
  InputDisabled,
  SendHeld,
  PreviewOpen,
  LayerAbove,
  PasteInFlight,
  Busy,
};

AttachRefusal RefusalFor(const AttachState& state);

/** The locale key of the pill that says why; nullptr = say nothing (locked, or a paste already being written). */
const char* AttachRefusalKey(AttachRefusal refusal);

enum class FileStatus { Regular, Directory, Missing, Error };
/** What a file is by its head (PNG / JPEG / GIF / WebP) or, for a video, by its name; anything else is Other. */
enum class SniffedType { Png, Jpeg, Gif, Webp, Video, Other };

/** What the worker learns about one path. */
struct FileFacts {
  FileStatus status = FileStatus::Error;
  uint64_t size = 0;
  SniffedType sniffed = SniffedType::Other;
  /** Width × height from the head of a picture; 0 when unknown. */
  uint64_t pixels = 0;
};

/** Blocking file system work (status, size, the head of the file): call it off the UI thread. */
FileFacts GatherFileFacts(const std::string& path);

struct AttachPlannedItem {
  std::string path;
  PictureSendKind kind = PictureSendKind::File;
  uint64_t bytes = 0;
  uint64_t pixels = 0;
  bool owned = false;
};

struct AttachPlan {
  std::vector<AttachPlannedItem> items;
  /** Offered folders, dropped. */
  int folders = 0;
  /** Missing, not a plain file, or not readable: dropped. */
  int unreadable = 0;
  /** More than `max_items` could be taken: the first ones were kept. */
  bool truncated = false;
  /** The owned candidates that did not make it into `items` (the caller deletes them). */
  std::vector<std::string> discarded_owned;
};

/** `facts[i]` belongs to `candidates[i]`. Dropped ones are counted, then the first `max_items` of the rest are kept. */
AttachPlan PlanAttachCandidates(const std::vector<AttachCandidate>& candidates, const std::vector<FileFacts>& facts,
                                size_t max_items);

/** What the screen says about a batch that is going on to the page: one pill at most (folders, then unreadable, then the cap). nullptr = nothing. */
const char* AttachPlanNoteKey(const AttachPlan& plan);

struct AttachIntakePorts {
  /** The state now (UI thread). */
  std::function<AttachState()> state;
  /** The open chat's id ("" = none) and a number that changes with every lock / unlock (UI thread). */
  std::function<std::string()> chat_id;
  std::function<uint64_t()> session;
  /** Run `work` on a worker thread / on the UI thread. */
  std::function<void(std::function<void()> work)> post_worker;
  std::function<void(std::function<void()> work)> post_ui;
  /** Defaults to GatherFileFacts. Worker thread. */
  std::function<FileFacts(const std::string& path)> gather;
  /** Show a pill: a locale key and, for the keys that count something, `n`. UI thread. */
  std::function<void(const char* key, int n)> say;
  /** Delete a file this app made for an attachment that is not used. Default: nothing (no owned source yet). */
  std::function<void(const std::string& path)> release_owned;
  /** The batch goes on to the page, for chat `thread_id`; false when the page could not open (the caller released the items). */
  std::function<bool(const std::string& thread_id, std::vector<AttachPlannedItem> items)> open_page;
};

/** Candidates in, send-preview page out (or a pill saying why not). Pure apart from the ports. */
class AttachIntake {
public:
  static constexpr size_t kMaxItems = PictureSendQueue::kMaxItems;

  explicit AttachIntake(AttachIntakePorts ports);

  /** Takes the candidates in the chat open now. Refused: a pill, owned files released. Else the facts are read on a worker and the page opens. */
  void Open(std::vector<AttachCandidate> candidates, AttachSource source);

private:
  void Release(const std::vector<AttachCandidate>& candidates);
  void Finish(const std::string& thread_id, uint64_t session, const std::vector<AttachCandidate>& candidates,
              const std::vector<FileFacts>& facts);

  AttachIntakePorts ports_;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace pbr
