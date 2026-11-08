#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace pbr {

/**
 * The chat of the most recent inbound messages, by message id, and of a call by call id (set when its summary
 * arrives). A notification tap names a message or call that may have synced before the tap; this answers where
 * it went. Oldest entries go first. Thread-safe: ingest runs on workers, the tap is read on the UI thread.
 */
class InboundThreadIndex {
public:
  static constexpr size_t kCapacity = 256;

  void Remember(std::string id, std::string thread_id) {
    if (id.empty() || thread_id.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (auto it = threads_.find(id); it != threads_.end()) {
      it->second = std::move(thread_id);
      return;
    }
    if (order_.size() >= kCapacity) {
      threads_.erase(order_.front());
      order_.pop_front();
    }
    order_.push_back(id);
    threads_.emplace(std::move(id), std::move(thread_id));
  }

  std::optional<std::string> Find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (auto it = threads_.find(id); it != threads_.end()) {
      return it->second;
    }
    return std::nullopt;
  }

private:
  mutable std::mutex mu_;
  std::deque<std::string> order_;
  std::unordered_map<std::string, std::string> threads_;
};

} // namespace pbr
