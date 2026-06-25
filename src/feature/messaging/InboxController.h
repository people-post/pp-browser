#pragma once

#include "common/Module.h"
#include "contacts/ContactsStore.h"
#include "demo/ChatWidgetTypes.h"
#include "messaging/IThreadStore.h"
#include "messaging/ThreadTypes.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

class InboxController : public Module {
public:
  InboxController(IThreadStore& store, ContactsStore& contacts);

  Roe<std::vector<Thread>> ListThreads();
  Roe<Thread> GetActiveThread() const;
  const std::string& ActiveThreadId() const { return active_thread_id_; }

  Roe<Thread> OpenThread(const std::string& thread_id);
  Roe<Thread> CreateAiHomeThread();
  Roe<Thread> CreateNewAiThread();
  Roe<Thread> CreateDirectThread(const std::string& contact_id);
  Roe<Thread> FindOrCreateDirectThread(const std::string& contact_id);

  bool IsAiHomeThread(const std::string& thread_id) const;
  Roe<void> CloseThread(const std::string& thread_id);

  void MarkThreadRead(const std::string& thread_id);
  Roe<void> UpdatePreview(const std::string& thread_id, const std::string& preview);

  std::vector<MessageDisplayRow> BuildDisplayRows(const std::string& thread_id) const;

  using ThreadChangedCallback = std::function<void()>;
  void SetOnThreadChanged(ThreadChangedCallback callback);

private:
  Roe<void> EnsureAiHomeThread();
  std::string ResolveSenderLabel(const std::string& sender_contact_id) const;
  std::string ResolveRowClass(const std::string& sender_contact_id) const;
  std::string BuildMessageRml(const ThreadMessage& message) const;

  IThreadStore& store_;
  ContactsStore& contacts_;
  std::string active_thread_id_;
  std::string ai_home_thread_id_;
  ThreadChangedCallback on_thread_changed_;
};

} // namespace pbr
