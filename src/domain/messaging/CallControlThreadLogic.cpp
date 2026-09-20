#include "domain/messaging/CallControlThreadLogic.h"

#include "domain/messaging/CallThreadPresenceLogic.h"
#include "common/directory/DirectoryJson.h"
#include "common/thread/ThreadChannel.h"
#include "common/thread/ThreadTypes.h"

namespace pbr {

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
  direct_target.peer_identity_kind = ContactIdKindToString(ContactIdKind::Account);
  direct_target.peer_identity_value = peer_identity;
  direct_target.channel = ThreadChannel::E2ePublic;

  const std::string title = dm_title.empty() ? peer_identity : dm_title;
  auto thread = store.FindOrCreateDirectThread(direct_target, contact_id, title);
  if (!thread) {
    return thread.error();
  }
  return thread->id;
}

Roe<size_t> PruneOrphanCallControlShadows(IThreadStore& store,
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
    auto page = store.GetMessagesPage(thread.id, std::nullopt, kOrphanCallControlShadowScanLimit);
    if (!page) {
      continue;
    }
    if (!TranscriptIsOnlyCallControl(*page, kOrphanCallControlShadowScanLimit)) {
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
