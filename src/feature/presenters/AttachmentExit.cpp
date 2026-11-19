#include "feature/presenters/AttachmentExit.h"

#include "common/Logger.h"
#include "common/PathUtf8.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/voice/AudioFilePlayer.h"
#include "lib/voice/VoiceFile.h"

#include <cstring>
#include <utility>

namespace pbr {

namespace {

logging::Logger log() {
  return logging::getLogger("attachment");
}

} // namespace

AttachmentExitKind KindOfAttachment(const std::string& mime, const std::string& filename, const uint64_t declared_bytes,
                                    const int voice_max_seconds, const bool voice_bubbles) {
  if (voice_bubbles && DecideVoiceBubble(mime, filename, declared_bytes, voice_max_seconds).bubble) {
    return AttachmentExitKind::Voice;
  }
  if (IsAttachmentImageMime(mime)) {
    return AttachmentExitKind::Picture;
  }
  if (IsAttachmentVideoMime(mime)) {
    return AttachmentExitKind::Video;
  }
  if (IsAudioFileAttachment(mime, filename)) {
    return AttachmentExitKind::AudioFile;
  }
  return AttachmentExitKind::File;
}

std::string SuggestedSaveName(const std::string& sender_filename, const std::string& safe_name) {
  if (!safe_name.empty()) {
    return safe_name;
  }
  constexpr size_t kMaxNameBytes = 100;
  const size_t slash = sender_filename.find_last_of("/\\");
  std::string name;
  for (const char ch : sender_filename.substr(slash == std::string::npos ? 0 : slash + 1)) {
    const auto c = static_cast<unsigned char>(ch);
    if (c >= 0x20 && c != 0x7F && std::strchr("<>:\"|?*", ch) == nullptr) {
      name.push_back(ch);
    }
  }
  const size_t first = name.find_first_not_of(". ");
  name = first == std::string::npos ? std::string() : name.substr(first);
  if (name.size() > kMaxNameBytes) {
    size_t cut = kMaxNameBytes;
    while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) {
      --cut; // not inside a UTF-8 character
    }
    name.resize(cut);
  }
  const size_t last = name.find_last_not_of(". ");
  name = last == std::string::npos ? std::string() : name.substr(0, last + 1);
  return name.empty() ? std::string("attachment") : name;
}

AttachmentTap DecideAttachmentTap(const AttachmentLayout layout, const AttachmentExitKind kind, const bool ready,
                                  const bool plays_in_app, const bool save_busy) {
  if (layout == AttachmentLayout::Phone) {
    return AttachmentTap::Open;
  }
  if (save_busy) {
    return AttachmentTap::Ignore;
  }
  switch (kind) {
    case AttachmentExitKind::Picture:
    case AttachmentExitKind::Video:
    case AttachmentExitKind::Voice:
      return AttachmentTap::Open;
    case AttachmentExitKind::AudioFile:
      if (plays_in_app) {
        return AttachmentTap::PlayInApp;
      }
      break;
    case AttachmentExitKind::File:
      break;
  }
  return ready ? AttachmentTap::Save : AttachmentTap::NotReady;
}

AttachmentMenuEntries DecideMenuEntries(const AttachmentLayout layout, const bool can_share_file,
                                        const std::optional<AttachmentExitKind> kind) {
  AttachmentMenuEntries entries;
  if (!kind || *kind == AttachmentExitKind::Voice) {
    return entries;
  }
  entries.save_as = layout == AttachmentLayout::Desktop;
  entries.share = can_share_file;
  return entries;
}

bool AttachmentSaveState::TryBegin(const std::string& message_id) {
  if (phase_ != Phase::Idle) {
    return false;
  }
  phase_ = Phase::Choosing;
  message_id_ = message_id;
  return true;
}

bool AttachmentSaveState::Chosen(const std::string& message_id) {
  if (phase_ != Phase::Choosing || message_id != message_id_) {
    return false;
  }
  phase_ = Phase::Saving;
  return true;
}

void AttachmentSaveState::Finish(const std::string& message_id) {
  if (phase_ != Phase::Idle && message_id == message_id_) {
    phase_ = Phase::Idle;
    message_id_.clear();
  }
}

AttachmentSaveFlow::Start AttachmentSaveFlow::Begin(const std::string& thread_id, const std::string& message_id,
                                                    const std::string& suggested_name, const bool ready) {
  if (state_.Busy()) {
    return Start::Ignored;
  }
  if (!ready) {
    ports_.fail(Tr("chat.attachment.not_ready"));
    return Start::NotReady;
  }
  state_.TryBegin(message_id);
  ports_.choose_path(suggested_name, [this, thread_id, message_id](SaveChoice choice) {
    // The dialog may answer on another thread: back on the UI thread before anything changes.
    ports_.post_ui(
        [this, thread_id, message_id, choice = std::move(choice)]() { OnChoice(thread_id, message_id, choice); });
  });
  return Start::Started;
}

void AttachmentSaveFlow::OnChoice(const std::string& thread_id, const std::string& message_id,
                                  const SaveChoice& choice) {
  if (choice.kind != SaveChoice::Kind::Chosen || choice.path.empty()) {
    state_.Finish(message_id);
    if (choice.kind == SaveChoice::Kind::Unavailable) {
      log().warning << "attachment save: no save dialog";
      ports_.fail(Tr("chat.attachment.save_unavailable"));
    } else {
      log().info << "attachment save: cancelled";
    }
    return;
  }
  if (!state_.Chosen(message_id)) {
    return;
  }
  ports_.ok(Tr("chat.attachment.saving"));
  ports_.post_worker([this, thread_id, message_id, target = choice.path]() {
    // The view is made (decrypted) only now that a path is chosen; no view (locked meanwhile) = no target file.
    const std::optional<std::string> view = ports_.view_path(thread_id, message_id);
    const bool copied = view && ports_.copy(*view, target);
    ports_.post_ui([this, message_id, target, copied]() { OnCopied(message_id, target, copied); });
  });
}

void AttachmentSaveFlow::OnCopied(const std::string& message_id, const std::string& target, const bool copied) {
  state_.Finish(message_id);
  if (!copied) {
    log().warning << "attachment save: failed";
    ports_.fail(Tr("chat.attachment.save_failed"));
    return;
  }
  ports_.mark_downloaded(target);
  log().info << "attachment save: done";
  ports_.ok(
      Tr("chat.attachment.saved_to", {{"folder", ports_.folder_name(PathToUtf8(PathFromUtf8(target).parent_path()))}}));
}

AttachmentShareFlow::Start AttachmentShareFlow::Begin(const std::string& thread_id, const std::string& message_id,
                                                      const std::string& mime, const bool ready, const int x,
                                                      const int y) {
  if (busy_) {
    return Start::Ignored;
  }
  if (!ready) {
    ports_.fail(Tr("chat.attachment.not_ready"));
    return Start::NotReady;
  }
  busy_ = true;
  ports_.post_worker([this, thread_id, message_id, mime, x, y]() {
    std::optional<std::string> view = ports_.view_path(thread_id, message_id);
    ports_.post_ui([this, mime, x, y, view = std::move(view)]() {
      busy_ = false;
      if (view && ports_.share(*view, mime, x, y)) {
        log().info << "attachment share: sheet shown";
        return;
      }
      log().warning << "attachment share: failed";
      ports_.fail(Tr("chat.attachment.share_failed"));
    });
  });
  return Start::Started;
}

} // namespace pbr
