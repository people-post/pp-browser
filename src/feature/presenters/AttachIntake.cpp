#include "feature/presenters/AttachIntake.h"

#include "common/ImageMetadata.h"
#include "common/PathUtf8.h"
#include "common/Logger.h"
#include "lib/messaging/AttachmentCache.h"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace pbr {

AttachRefusal RefusalFor(const AttachState& state) {
  if (!state.ready) {
    return AttachRefusal::NotReady;
  }
  if (!state.chat_open) {
    return AttachRefusal::NoChat;
  }
  if (state.attach_unavailable) {
    return AttachRefusal::AttachUnavailable;
  }
  if (state.ai_composer) {
    return AttachRefusal::AiComposer;
  }
  if (state.editing_message) {
    return AttachRefusal::Editing;
  }
  if (state.input_disabled) {
    return AttachRefusal::InputDisabled;
  }
  if (state.send_held) {
    return AttachRefusal::SendHeld;
  }
  if (state.preview_open) {
    return AttachRefusal::PreviewOpen;
  }
  if (state.layer_above) {
    return AttachRefusal::LayerAbove;
  }
  if (state.paste_in_flight) {
    return AttachRefusal::PasteInFlight;
  }
  if (state.attachment_busy) {
    return AttachRefusal::Busy;
  }
  return AttachRefusal::None;
}

const char* AttachRefusalKey(const AttachRefusal refusal) {
  switch (refusal) {
    case AttachRefusal::NoChat:
      return "chat.attach.no_chat";
    case AttachRefusal::AttachUnavailable:
    case AttachRefusal::AiComposer:
    case AttachRefusal::Editing:
    case AttachRefusal::InputDisabled:
    case AttachRefusal::LayerAbove:
      return "chat.attach.not_here";
    case AttachRefusal::SendHeld:
      return "chat.send.held";
    case AttachRefusal::PreviewOpen:
      return "chat.attach.finish_current";
    case AttachRefusal::Busy:
      return "chat.attach.previous_still_sending";
    case AttachRefusal::None:
    case AttachRefusal::NotReady:
    case AttachRefusal::PasteInFlight:
      break;
  }
  return nullptr;
}

DropVerdict DecideDrop(const bool has_position, const bool inside_chat, const AttachRefusal refusal) {
  if (!has_position) {
    return DropVerdict::NoPosition;
  }
  if (!inside_chat) {
    return DropVerdict::Outside;
  }
  return refusal == AttachRefusal::None ? DropVerdict::Take : DropVerdict::Refuse;
}

PasteAction DecidePaste(const ClipboardSummary& clipboard, const AttachRefusal refusal) {
  PasteAction action;
  if (clipboard.has_files && clipboard.files_enabled) {
    // Copied files win over text (Finder also puts the file's name there) and over a picture (its icon).
    action.take_over = true;
    if (refusal != AttachRefusal::None) {
      action.pill_key = AttachRefusalKey(refusal);
    } else {
      action.read_files = true;
    }
    return action;
  }
  if (clipboard.has_files || clipboard.has_text) {
    return action; // files with the switch off: left to the text field (D1); text always goes to the field, a picture beside it or not
  }
  const bool readable = !clipboard.image_type.empty();
  if (!readable && !clipboard.has_unusable_image) {
    return action;
  }
  action.take_over = true;
  if (refusal != AttachRefusal::None) {
    action.pill_key = AttachRefusalKey(refusal);
  } else if (readable) {
    action.read_image = true;
  } else {
    action.pill_key = "chat.paste.unsupported";
  }
  return action;
}

std::string DescribePasteDecision(const ClipboardSummary& clipboard, const PasteAction& action) {
  return std::string("paste: files=") + (clipboard.has_files ? "1" : "0") +
         " text=" + (clipboard.has_text ? "1" : "0") +
         " image=" + (clipboard.image_type.empty() ? "none" : clipboard.image_type) +
         " unusable=" + (clipboard.has_unusable_image ? "1" : "0") + " -> " +
         (action.take_over ? "take over" : "leave to the text field");
}

const char* DropHintKey(const bool inside_chat, const AttachRefusal refusal) {
  if (!inside_chat) {
    return nullptr;
  }
  return refusal == AttachRefusal::None ? "chat.drop.hint" : AttachRefusalKey(refusal);
}

FileFacts GatherFileFacts(const std::string& path) {
  FileFacts facts;
  std::error_code ec;
  const std::filesystem::file_status status = std::filesystem::status(PathFromUtf8(path), ec);
  if (status.type() == std::filesystem::file_type::not_found) {
    facts.status = FileStatus::Missing;
    return facts;
  }
  if (ec) {
    facts.status = FileStatus::Error;
    return facts;
  }
  if (status.type() == std::filesystem::file_type::directory) {
    facts.status = FileStatus::Directory;
    return facts;
  }
  if (status.type() != std::filesystem::file_type::regular) {
    facts.status = FileStatus::Error; // a device, a pipe, a socket: nothing to send
    return facts;
  }
  facts.status = FileStatus::Regular;
  const auto size = std::filesystem::file_size(PathFromUtf8(path), ec);
  if (ec) {
    facts.status = FileStatus::Error;
    return facts;
  }
  facts.size = static_cast<uint64_t>(size);
  switch (ImageKindOfFile(path)) {
    case ImageKind::Png:
      facts.sniffed = SniffedType::Png;
      break;
    case ImageKind::Jpeg:
      facts.sniffed = SniffedType::Jpeg;
      break;
    case ImageKind::Gif:
      facts.sniffed = SniffedType::Gif;
      break;
    case ImageKind::Webp:
      facts.sniffed = SniffedType::Webp;
      break;
    case ImageKind::Unknown:
      facts.sniffed =
          AttachmentMimeFromFilename(path).rfind("video/", 0) == 0 ? SniffedType::Video : SniffedType::Other;
      break;
  }
  if (facts.sniffed != SniffedType::Video && facts.sniffed != SniffedType::Other) {
    if (const auto header = ReadImageHeader(path)) {
      facts.pixels = static_cast<uint64_t>(header->width) * static_cast<uint64_t>(header->height);
    }
  }
  return facts;
}

AttachPlan PlanAttachCandidates(const std::vector<AttachCandidate>& candidates, const std::vector<FileFacts>& facts,
                                const size_t max_items) {
  AttachPlan plan;
  for (size_t i = 0; i < candidates.size(); ++i) {
    const AttachCandidate& candidate = candidates[i];
    const FileStatus status = i < facts.size() ? facts[i].status : FileStatus::Error;
    if (status == FileStatus::Directory) {
      ++plan.folders;
    } else if (status != FileStatus::Regular) {
      ++plan.unreadable;
    } else if (plan.items.size() >= max_items) {
      plan.truncated = true;
    } else {
      const FileFacts& fact = facts[i];
      AttachPlannedItem item;
      item.path = candidate.path;
      item.bytes = fact.size;
      item.pixels = fact.pixels;
      item.owned = candidate.owned;
      item.display_name = candidate.display_name;
      switch (fact.sniffed) {
        case SniffedType::Png:
        case SniffedType::Jpeg:
        case SniffedType::Gif:
        case SniffedType::Webp:
          item.kind = PictureSendKind::Picture;
          break;
        case SniffedType::Video:
          item.kind = PictureSendKind::Video;
          break;
        case SniffedType::Other:
          item.kind = PictureSendKind::File;
          break;
      }
      plan.items.push_back(std::move(item));
      continue;
    }
    if (candidate.owned) {
      plan.discarded_owned.push_back(candidate.path);
    }
  }
  return plan;
}

const char* AttachPlanNoteKey(const AttachPlan& plan) {
  if (plan.folders > 0) {
    return "chat.attach.no_folders";
  }
  if (plan.unreadable > 0) {
    return "chat.attach.some_unreadable";
  }
  if (plan.truncated) {
    return "chat.attach.max_items";
  }
  return nullptr;
}

AttachIntake::AttachIntake(AttachIntakePorts ports) : ports_(std::move(ports)) {}

void AttachIntake::Release(const std::vector<AttachCandidate>& candidates) {
  if (!ports_.release_owned) {
    return;
  }
  for (const AttachCandidate& candidate : candidates) {
    if (candidate.owned) {
      ports_.release_owned(candidate.path);
    }
  }
}

void AttachIntake::Open(std::vector<AttachCandidate> candidates, const AttachSource /*source*/) {
  if (candidates.empty()) {
    return;
  }
  const AttachRefusal refusal = RefusalFor(ports_.state());
  if (refusal != AttachRefusal::None) {
    if (const char* key = AttachRefusalKey(refusal); key && ports_.say) {
      ports_.say(key, 0);
    }
    Release(candidates);
    return;
  }
  const std::string thread_id = ports_.chat_id();
  const uint64_t session = ports_.session();
  const std::weak_ptr<int> alive = alive_;
  auto gather = ports_.gather ? ports_.gather : std::function<FileFacts(const std::string&)>(&GatherFileFacts);
  auto post_ui = ports_.post_ui;
  auto release = ports_.release_owned;
  // The file system (a network drive or a cloud placeholder can be slow) is asked off the UI thread. The worker never
  // touches this intake (it may be gone by then): everything it needs is copied in, as PasteAttach does.
  ports_.post_worker([this, alive, gather = std::move(gather), post_ui = std::move(post_ui),
                      release = std::move(release), candidates = std::move(candidates), thread_id, session]() mutable {
    std::vector<FileFacts> facts;
    facts.reserve(candidates.size());
    for (const AttachCandidate& candidate : candidates) {
      facts.push_back(gather(candidate.path));
    }
    post_ui([this, alive, release = std::move(release), candidates = std::move(candidates), facts = std::move(facts),
             thread_id, session]() {
      if (alive.expired()) {
        if (release) {
          for (const AttachCandidate& candidate : candidates) {
            if (candidate.owned) {
              release(candidate.path); // the intake is gone: the files this app made for the batch go with it
            }
          }
        }
        return;
      }
      Finish(thread_id, session, candidates, facts);
    });
  });
}

void AttachIntake::Finish(const std::string& thread_id, const uint64_t session,
                          const std::vector<AttachCandidate>& candidates, const std::vector<FileFacts>& facts) {
  // Whatever happened while the worker read the files: the same chat, the same unlocked session, and still allowed.
  const AttachRefusal refusal = RefusalFor(ports_.state());
  if (refusal != AttachRefusal::None || thread_id != ports_.chat_id() || session != ports_.session()) {
    const char* key = refusal != AttachRefusal::None ? AttachRefusalKey(refusal) : "chat.attach.not_sent_left_chat";
    if (key && ports_.say) {
      ports_.say(key, 0);
    }
    Release(candidates);
    return;
  }
  AttachPlan plan = PlanAttachCandidates(candidates, facts, kMaxItems);
  if (ports_.release_owned) {
    for (const std::string& path : plan.discarded_owned) {
      ports_.release_owned(path);
    }
  }
  if (const char* key = AttachPlanNoteKey(plan); key && ports_.say) {
    ports_.say(key, plan.folders > 0 ? plan.folders : plan.unreadable);
  }
  if (plan.items.empty()) {
    return;
  }
  // From here open_page owns the items, whatever it answers: the queue releases what it took, the opener what it never
  // gave to the queue. Releasing them here too would delete a reused name's new file (N1).
  ports_.open_page(thread_id, std::move(plan.items));
}

PasteAttach::PasteAttach(PastePorts ports) : ports_(std::move(ports)) {}

bool PasteAttach::TryPaste() {
  // While a picture is being written another paste of a picture or files is swallowed quietly (RefusalFor's
  // PasteInFlight says nothing), but plain text still goes to the text field: DecidePaste only takes over what is ours.
  const ClipboardSummary clipboard = ports_.peek();
  AttachState state = ports_.state();
  state.paste_in_flight = in_flight_;
  const PasteAction action = DecidePaste(clipboard, RefusalFor(state));
  logging::getLogger("Platform.Paste").debug << DescribePasteDecision(clipboard, action);
  if (!action.take_over) {
    return false;
  }
  if (action.pill_key && ports_.say) {
    ports_.say(action.pill_key, 0);
  }
  if (action.read_files) {
    std::vector<AttachCandidate> candidates;
    for (std::string& path : ports_.read_files ? ports_.read_files() : std::vector<std::string>()) {
      candidates.push_back(AttachCandidate{std::move(path), false, {}}); // the user's own files: never deleted by us
    }
    if (candidates.empty()) {
      if (ports_.has_text && ports_.has_text()) {
        return false; // the files read back as nothing (gone, or a stale type list): the text on the clipboard pastes as text
      }
      if (ports_.say) {
        ports_.say("chat.paste.failed", 0); // gone since the peek, or could not be read
      }
      return true;
    }
    ports_.open_preview(std::move(candidates));
    return true;
  }
  if (!action.read_image) {
    return true;
  }
  std::optional<ClipboardImage> image = ports_.read_image();
  if (!image) {
    if (ports_.say) {
      ports_.say("chat.paste.unsupported", 0); // gone since the peek, or not really a PNG
    }
    return true;
  }
  if (image->too_large) {
    if (ports_.say) {
      ports_.say("chat.paste.too_large", 0);
    }
    return true;
  }
  in_flight_ = true;
  const std::string thread_id = ports_.chat_id();
  const uint64_t session = ports_.session();
  const std::weak_ptr<int> alive = alive_;
  auto write = ports_.write;
  auto release = ports_.release_owned;
  auto post_ui = ports_.post_ui;
  ports_.post_worker([this, alive, write = std::move(write), release = std::move(release), post_ui = std::move(post_ui),
                      thread_id, session, bytes = std::move(image->bytes), extension = image->extension]() {
    std::optional<std::string> path = write(bytes, extension);
    post_ui([this, alive, release, path = std::move(path), thread_id, session]() {
      if (alive.expired()) {
        if (path && release) {
          release(*path);
        }
        return;
      }
      in_flight_ = false; // first, whatever came out of the write
      if (!path) {
        if (ports_.say) {
          ports_.say("chat.paste.failed", 0);
        }
        return;
      }
      if (thread_id != ports_.chat_id() || session != ports_.session()) {
        if (ports_.release_owned) {
          ports_.release_owned(*path); // the chat changed or the app locked meanwhile: no page
        }
        return;
      }
      ports_.open_preview({AttachCandidate{*path, true, kDisplayName}});
    });
  });
  return true;
}

} // namespace pbr
