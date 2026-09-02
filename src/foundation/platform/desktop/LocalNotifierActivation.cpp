#include "foundation/platform/desktop/LocalNotifierImpl.h"

#include <chrono>
#include <mutex>

namespace pbr::desktop {
namespace {

std::mutex g_activation_mutex;
NotificationActivationFn g_activation_handler;
std::string g_pending_token;
std::chrono::steady_clock::time_point g_pending_token_at{};

} // namespace

void SetDesktopNotificationActivationHandler(NotificationActivationFn handler) {
  std::lock_guard<std::mutex> lock(g_activation_mutex);
  g_activation_handler = std::move(handler);
}

void SetPendingDesktopActivationToken(std::string token) {
  std::lock_guard<std::mutex> lock(g_activation_mutex);
  g_pending_token = std::move(token);
  g_pending_token_at = std::chrono::steady_clock::now();
}

std::string TakePendingDesktopActivationToken() {
  std::lock_guard<std::mutex> lock(g_activation_mutex);
  if (g_pending_token.empty()) {
    return {};
  }
  const auto age = std::chrono::steady_clock::now() - g_pending_token_at;
  if (age > std::chrono::seconds(2)) {
    g_pending_token.clear();
    return {};
  }
  std::string token = std::move(g_pending_token);
  g_pending_token.clear();
  return token;
}

void DispatchDesktopNotificationActivation(const std::string& thread_id) {
  NotificationActivationFn handler;
  {
    std::lock_guard<std::mutex> lock(g_activation_mutex);
    handler = g_activation_handler;
  }
  if (handler) {
    handler(thread_id);
  }
}

} // namespace pbr::desktop
