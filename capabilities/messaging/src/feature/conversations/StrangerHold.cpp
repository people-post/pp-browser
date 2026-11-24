#include "feature/conversations/StrangerHold.h"

#include "lib/delivery/DeliveryNotice.h"

namespace pbr {

bool StrangerHoldActive(const ThreadMessage& line, const std::optional<ThreadMessage>& probe) {
  const std::optional<std::string> probe_id = StrangerHeldRowProbe(line);
  return probe_id && probe && probe->id == *probe_id && probe->delivery == MessageDelivery::Pending;
}

std::optional<std::string> ActiveStrangerHold(const IThreadStore& store, const std::string& thread_id) {
  auto queued = store.ListPendingOutbox();
  if (!queued) {
    return std::nullopt;
  }
  for (const auto& [message_id, queued_thread] : *queued) {
    if (queued_thread != thread_id) {
      continue;
    }
    auto line = store.GetMessage(thread_id, StrangerHeldRowId(message_id));
    if (!line || !*line) {
      continue;
    }
    auto probe = store.GetMessage(thread_id, message_id);
    if (probe && StrangerHoldActive(**line, *probe)) {
      return message_id;
    }
  }
  return std::nullopt;
}

bool StrangerHeldLineShown(const IThreadStore& store, const ThreadMessage& line) {
  const std::optional<std::string> probe_id = StrangerHeldRowProbe(line);
  if (!probe_id) {
    return false;
  }
  auto probe = store.GetMessage(line.thread_id, *probe_id);
  return probe && StrangerHoldActive(line, *probe);
}

} // namespace pbr
