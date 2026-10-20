#include "domain/messaging/ControlThreadLogic.h"

#include "common/directory/DirectoryJson.h"
#include "common/thread/ThreadChannel.h"
#include "common/ValueJson.h"
#include "common/MeshAlias.h"
#include "common/PbrCompat.h"

namespace pbr {

bool IsSystemControlMessage(const ThreadMessage& message) {
  if (message.content_type != ChatContentType::System) {
    return false;
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return false;
  }
  const auto control_type = payload->getString("control_type");
  return control_type && !control_type->empty() && *control_type != "system";
}

Roe<std::string> ResolveOrCreateE2ePublicDirectThread(IThreadStore& store,
                                                     const std::string& peer_identity,
                                                     const std::optional<std::string>& prefer_thread_id,
                                                     const std::string& contact_id,
                                                     const std::string& dm_title) {
  if (peer_identity.empty()) {
    return Error("Peer identity required");
  }
  if (prefer_thread_id && !prefer_thread_id->empty()) {
    if (auto origin = store.GetThread(*prefer_thread_id); origin && *origin) {
      const Thread& thr = **origin;
      if (thr.kind == ThreadKind::Direct && thr.channel == ThreadChannel::E2ePublic &&
          thr.peer_identity_value == peer_identity) {
        return thr.id;
      }
    }
  }

  DirectChatTarget direct_target;
  direct_target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  direct_target.peer_identity_value = peer_identity;
  direct_target.channel = ThreadChannel::E2ePublic;

  const std::string title = dm_title.empty() ? peer_identity : dm_title;
  auto thread = store.FindOrCreateDirectThread(direct_target, contact_id, title);
  if (!thread) {
    return thread.error();
  }
  return thread->id;
}

bool HasPrivateE2eSibling(const Thread& thread, const std::vector<Thread>& all_threads) {
  if (thread.kind != ThreadKind::Direct || thread.channel != ThreadChannel::E2ePublic) {
    return false;
  }
  if (thread.peer_identity_value.empty()) {
    return false;
  }
  for (const Thread& other : all_threads) {
    if (other.id == thread.id || other.kind != ThreadKind::Direct) {
      continue;
    }
    if (other.peer_identity_value != thread.peer_identity_value) {
      continue;
    }
    if (other.channel == ThreadChannel::E2e) {
      return true;
    }
  }
  return false;
}

bool IsControlShadowThread(const Thread& thread, const std::vector<Thread>& all_threads) {
  if (!thread.preview.empty() || thread.unread_count != 0) {
    return false;
  }
  return HasPrivateE2eSibling(thread, all_threads);
}

bool TranscriptIsOnlyControl(const std::vector<ThreadMessage>& messages, size_t scan_limit) {
  if (scan_limit == 0 || messages.size() >= scan_limit) {
    return false;
  }
  for (const ThreadMessage& message : messages) {
    if (!IsSystemControlMessage(message)) {
      return false;
    }
  }
  return true;
}

Roe<size_t> PruneOrphanControlShadows(IThreadStore& store,
                                          const std::unordered_set<std::string>& protect_thread_ids) {
  auto threads = store.ListThreads();
  if (!threads) {
    return threads.error();
  }

  size_t deleted = 0;
  for (const Thread& thread : *threads) {
    if (protect_thread_ids.count(thread.id) != 0) {
      continue;
    }
    if (!HasPrivateE2eSibling(thread, *threads)) {
      continue;
    }
    auto page = store.GetMessagesPage(thread.id, std::nullopt, kOrphanControlShadowScanLimit);
    if (!page) {
      continue;
    }
    if (!TranscriptIsOnlyControl(*page, kOrphanControlShadowScanLimit)) {
      continue;
    }
    auto removed = store.DeleteThread(thread.id);
    if (removed && *removed) {
      ++deleted;
    }
  }
  return deleted;
}

} // namespace pbr
