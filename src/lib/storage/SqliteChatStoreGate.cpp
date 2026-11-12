#include "lib/storage/SqliteChatStoreGate.h"

#include "foundation/error/AppError.h"

#include <mutex>
#include <unordered_map>

namespace pbr::chat_store_gate {

namespace {

struct Entry {
  int holders = 0;
  bool open = false;
  std::optional<ChatStoreOpenFailure> failure;
};

std::mutex& Mutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, Entry>& Entries() {
  static std::unordered_map<std::string, Entry> entries;
  return entries;
}

} // namespace

void Hold(const std::string& profile_db_path) {
  std::lock_guard lock(Mutex());
  Entry& entry = Entries()[profile_db_path];
  ++entry.holders;
  entry.failure.reset(); // a new store tries afresh (after Reset chats, say)
}

void Release(const std::string& profile_db_path) {
  std::lock_guard lock(Mutex());
  auto it = Entries().find(profile_db_path);
  if (it != Entries().end() && --it->second.holders <= 0) {
    Entries().erase(it);
  }
}

void MarkOpen(const std::string& profile_db_path) {
  std::lock_guard lock(Mutex());
  auto it = Entries().find(profile_db_path);
  if (it != Entries().end()) {
    it->second.open = true;
    it->second.failure.reset();
  }
}

void MarkFailed(const std::string& profile_db_path, ChatStoreOpenFailure failure) {
  std::lock_guard lock(Mutex());
  auto it = Entries().find(profile_db_path);
  if (it != Entries().end()) {
    it->second.open = false;
    it->second.failure = std::move(failure);
  }
}

Roe<void> RequireOpen(const std::string& profile_db_path) {
  std::lock_guard lock(Mutex());
  auto it = Entries().find(profile_db_path);
  if (it == Entries().end() || it->second.open) {
    return {};
  }
  if (it->second.failure) {
    return AppError::Storage(Err::Storage::Unavailable, it->second.failure->message);
  }
  return AppError::Pin(Err::Pin::Required, "The chat store opens after the profile is unlocked");
}

std::optional<ChatStoreOpenFailure> OpenFailure(const std::string& profile_db_path) {
  std::lock_guard lock(Mutex());
  auto it = Entries().find(profile_db_path);
  if (it == Entries().end()) {
    return std::nullopt;
  }
  return it->second.failure;
}

} // namespace pbr::chat_store_gate
