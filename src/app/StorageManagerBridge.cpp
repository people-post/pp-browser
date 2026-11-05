#include "app/StorageManagerBridge.h"

#include "feature/settings/StorageManagerModel.h"
#include "common/runtime/AppRuntime.h"

#include <utility>

namespace pbr {

StorageManagerPorts MakeStorageManagerPorts(ConversationsFacade& facade,
                                            std::function<std::string()> profile_data_dir) {
  StorageManagerPorts ports;
  ports.list_stored = [&facade](std::function<void(std::vector<StoredAttachmentItem>)> done) {
    facade.ListStoredAttachmentsAsync(std::move(done));
  };
  ports.releasable_bytes = [&facade](std::vector<StoredAttachmentRef> items, std::function<void(uint64_t)> done) {
    facade.ReleasableAttachmentBytesAsync(std::move(items), std::move(done));
  };
  ports.delete_local = [&facade](std::vector<StoredAttachmentRef> items,
                                 std::function<void(StoredAttachmentDeleteResult)> done) {
    facade.DeleteLocalAttachmentsAsync(std::move(items), std::move(done));
  };
  ports.message_store_bytes = [&facade](std::function<void(uint64_t)> done) {
    facade.MessageStoreBytesAsync(std::move(done));
  };
  ports.profile_bytes = [profile_data_dir = std::move(profile_data_dir)](std::function<void(uint64_t)> done) {
    const std::string dir = profile_data_dir ? profile_data_dir() : std::string();
    AppRuntime::PostWorkerAndReplyOnUI<uint64_t>(
        WorkerLane::Normal, [dir]() -> uint64_t { return dir.empty() ? 0 : DirectoryByteSize(dir); },
        [done = std::move(done)](const uint64_t bytes) {
          if (done) {
            done(bytes);
          }
        });
  };
  ports.chat_names = [&facade]() {
    std::vector<StorageChatName> names;
    auto threads = facade.ListThreads();
    if (!threads) {
      return names;
    }
    names.reserve(threads->size());
    for (const Thread& thread : *threads) {
      const std::string seed =
          thread.kind == ThreadKind::Direct && !thread.peer_identity_value.empty() ? thread.peer_identity_value
                                                                                   : thread.id;
      names.push_back({.thread_id = thread.id, .name = facade.ResolveThreadLabel(thread).title, .avatar_seed = seed});
    }
    return names;
  };
  ports.request_thumbnail = [&facade](const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                                      const std::string& mime, const uint64_t page_generation) {
    facade.RequestGridThumbnail(thread_id, content_hash, mime, page_generation);
  };
  ports.cancel_thumbnails = [&facade](const uint64_t page_generation) { facade.CancelGridThumbnails(page_generation); };
  ports.thumbnail_path = [&facade](const std::string& thread_id, const std::vector<uint8_t>& content_hash) {
    return facade.GridThumbnailPath(thread_id, content_hash);
  };
  ports.open_item = [&facade](const StoredAttachmentItem& item,
                              std::function<void(std::string path, bool needs_confirm)> done) {
    // Like a tap in the chat: the decrypt of a stored copy is blocking work, off the UI thread.
    AppRuntime::PostWorkerNormal([&facade, item, done = std::move(done)]() mutable {
      std::string path;
      bool needs_confirm = false;
      if (auto view = facade.AttachmentLocalPathForMessage(item.thread_id, item.message_id)) {
        path = std::move(*view);
        needs_confirm = facade.AttachmentOpenNeedsConfirmForMessage(item.thread_id, item.message_id);
      } else if (auto image = facade.AiImageViewPath(item.thread_id, item.content_hash, item.mime)) {
        path = std::move(*image); // a picture asked to the AI: no attachment message to read
      }
      AppRuntime::PostUI([done = std::move(done), path = std::move(path), needs_confirm]() mutable {
        if (done) {
          done(std::move(path), needs_confirm);
        }
      });
    });
  };
  ports.set_on_changed = [&facade](std::function<void()> on_changed) {
    facade.SetOnStoredAttachmentsChanged(std::move(on_changed));
  };
  ports.set_on_thumbnail_ready =
      [&facade](std::function<void(const std::string& thread_id, const std::vector<uint8_t>& content_hash)> on_ready) {
        facade.SetOnGridThumbnailReady(std::move(on_ready));
      };
  ports.run_later = [](const std::chrono::milliseconds delay, std::function<void()> task) {
    AppRuntime::ScheduleCoordinatorOneShot(delay, [task = std::move(task)]() mutable {
      AppRuntime::PostUI(std::move(task));
    });
  };
  return ports;
}

} // namespace pbr
