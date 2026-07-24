#if !defined(_WIN32) && !defined(__APPLE__)

#include "base/platform/desktop/LocalNotifierImpl.h"

#include "base/platform/ProductBranding.h"
#include "common/Logger.h"

#include <dbus/dbus.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace pbr::desktop {
namespace {

// Concurrency model:
// - Callers (often the UI thread) only enqueue posts/closes under g_mu.
// - The watch thread owns all dbus_connection_* I/O. Never call
//   send_with_reply_and_block (or any dbus I/O) from the UI thread — that
//   previously hung the app when competing with the watcher's read_write.

constexpr const char* kNotifyNode = "org.freedesktop.Notifications";
constexpr const char* kNotifyPath = "/org/freedesktop/Notifications";
constexpr const char* kNotifyIface = "org.freedesktop.Notifications";

struct PendingPost {
  std::string title;
  std::string body;
  std::string thread_id;
};

std::mutex g_mu;
DBusConnection* g_conn = nullptr;
std::unordered_map<std::string, uint32_t> g_thread_to_id;
std::unordered_map<uint32_t, std::string> g_id_to_thread;
std::vector<std::string> g_pending_activations;
std::vector<uint32_t> g_pending_closes;
std::vector<PendingPost> g_pending_posts;
std::atomic<bool> g_running{false};
std::thread g_watch_thread;
bool g_init_started = false;
bool g_init_ok = false;
bool g_logged_fail = false;

logging::Logger Log() {
  return logging::getLogger("LocalNotifier");
}

void LogFailOnce(const char* detail) {
  if (g_logged_fail) {
    return;
  }
  g_logged_fail = true;
  Log().warning << "Desktop notifications unavailable: " << (detail ? detail : "unknown");
}

void CloseNotificationId(DBusConnection* conn, dbus_uint32_t id) {
  DBusMessage* msg =
      dbus_message_new_method_call(kNotifyNode, kNotifyPath, kNotifyIface, "CloseNotification");
  if (!msg) {
    return;
  }
  if (!dbus_message_append_args(msg, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID)) {
    dbus_message_unref(msg);
    return;
  }
  dbus_uint32_t serial = 0;
  dbus_connection_send(conn, msg, &serial);
  dbus_connection_flush(conn);
  dbus_message_unref(msg);
}

bool AppendHintString(DBusMessageIter* hints, const char* key, const char* value) {
  DBusMessageIter entry;
  DBusMessageIter variant;
  if (!dbus_message_iter_open_container(hints, DBUS_TYPE_DICT_ENTRY, nullptr, &entry)) {
    return false;
  }
  if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key)) {
    return false;
  }
  if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant)) {
    return false;
  }
  if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value)) {
    return false;
  }
  if (!dbus_message_iter_close_container(&entry, &variant)) {
    return false;
  }
  return dbus_message_iter_close_container(hints, &entry) != FALSE;
}

bool AppendHintByte(DBusMessageIter* hints, const char* key, uint8_t value) {
  DBusMessageIter entry;
  DBusMessageIter variant;
  if (!dbus_message_iter_open_container(hints, DBUS_TYPE_DICT_ENTRY, nullptr, &entry)) {
    return false;
  }
  if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key)) {
    return false;
  }
  if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "y", &variant)) {
    return false;
  }
  if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_BYTE, &value)) {
    return false;
  }
  if (!dbus_message_iter_close_container(&entry, &variant)) {
    return false;
  }
  return dbus_message_iter_close_container(hints, &entry) != FALSE;
}

bool AppendHintInt32(DBusMessageIter* hints, const char* key, int32_t value) {
  DBusMessageIter entry;
  DBusMessageIter variant;
  if (!dbus_message_iter_open_container(hints, DBUS_TYPE_DICT_ENTRY, nullptr, &entry)) {
    return false;
  }
  if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key)) {
    return false;
  }
  if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "i", &variant)) {
    return false;
  }
  if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &value)) {
    return false;
  }
  if (!dbus_message_iter_close_container(&entry, &variant)) {
    return false;
  }
  return dbus_message_iter_close_container(hints, &entry) != FALSE;
}

DBusHandlerResult FilterMessage(DBusConnection* /*conn*/, DBusMessage* message, void* /*user_data*/) {
  if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL) {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  const char* iface = dbus_message_get_interface(message);
  const char* member = dbus_message_get_member(message);
  if (!iface || !member || std::string(iface) != kNotifyIface) {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  if (std::string(member) == "ActivationToken") {
    dbus_uint32_t id = 0;
    char* token = nullptr;
    DBusError err;
    dbus_error_init(&err);
    if (dbus_message_get_args(message, &err, DBUS_TYPE_UINT32, &id, DBUS_TYPE_STRING, &token,
                              DBUS_TYPE_INVALID) &&
        token && *token) {
      SetPendingDesktopActivationToken(token);
      Log().info << "ActivationToken id=" << id;
    } else if (dbus_error_is_set(&err)) {
      dbus_error_free(&err);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  if (std::string(member) == "NotificationClosed") {
    dbus_uint32_t id = 0;
    dbus_uint32_t reason = 0;
    DBusError err;
    dbus_error_init(&err);
    if (dbus_message_get_args(message, &err, DBUS_TYPE_UINT32, &id, DBUS_TYPE_UINT32, &reason,
                              DBUS_TYPE_INVALID)) {
      std::lock_guard<std::mutex> lock(g_mu);
      const auto it = g_id_to_thread.find(id);
      if (it != g_id_to_thread.end()) {
        g_thread_to_id.erase(it->second);
        g_id_to_thread.erase(it);
      }
    } else if (dbus_error_is_set(&err)) {
      dbus_error_free(&err);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  if (std::string(member) != "ActionInvoked") {
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  dbus_uint32_t id = 0;
  char* action = nullptr;
  DBusError err;
  dbus_error_init(&err);
  if (!dbus_message_get_args(message, &err, DBUS_TYPE_UINT32, &id, DBUS_TYPE_STRING, &action,
                             DBUS_TYPE_INVALID)) {
    Log().warning << "ActionInvoked parse failed";
    if (dbus_error_is_set(&err)) {
      dbus_error_free(&err);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  std::string thread_id;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    const auto it = g_id_to_thread.find(id);
    if (it != g_id_to_thread.end()) {
      thread_id = it->second;
    }
    g_pending_activations.push_back(thread_id);
  }
  Log().info << "ActionInvoked id=" << id << " action=" << (action ? action : "")
             << " thread=" << thread_id;
  return DBUS_HANDLER_RESULT_HANDLED;
}

void SendNotify(DBusConnection* conn, const PendingPost& post) {
  dbus_uint32_t replaces = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!post.thread_id.empty()) {
      const auto it = g_thread_to_id.find(post.thread_id);
      if (it != g_thread_to_id.end()) {
        replaces = it->second;
      }
    }
  }

  DBusMessage* msg = dbus_message_new_method_call(kNotifyNode, kNotifyPath, kNotifyIface, "Notify");
  if (!msg) {
    LogFailOnce("dbus Notify alloc failed");
    return;
  }

  const char* app_name = kProductName;
  const char* app_icon = "";
  const char* summary = post.title.c_str();
  const char* body_c = post.body.c_str();
  const dbus_int32_t expire_ms = -1;

  DBusMessageIter args;
  dbus_message_iter_init_append(msg, &args);
  if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &app_name) ||
      !dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &replaces) ||
      !dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &app_icon) ||
      !dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &summary) ||
      !dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &body_c)) {
    dbus_message_unref(msg);
    LogFailOnce("dbus Notify build failed");
    return;
  }

  DBusMessageIter actions;
  if (!dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &actions)) {
    dbus_message_unref(msg);
    Log().warning << "Post failed: open actions";
    return;
  }
  const char* default_key = "default";
  const char* default_label = "Open";
  if (!dbus_message_iter_append_basic(&actions, DBUS_TYPE_STRING, &default_key) ||
      !dbus_message_iter_append_basic(&actions, DBUS_TYPE_STRING, &default_label) ||
      !dbus_message_iter_close_container(&args, &actions)) {
    dbus_message_unref(msg);
    Log().warning << "Post failed: build actions";
    return;
  }

  DBusMessageIter hints;
  if (!dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &hints)) {
    dbus_message_unref(msg);
    Log().warning << "Post failed: open hints";
    return;
  }
  const uint8_t urgency = 1;
  const dbus_int32_t sender_pid = static_cast<dbus_int32_t>(getpid());
  if (!AppendHintByte(&hints, "urgency", urgency) ||
      !AppendHintString(&hints, "category", "im.received") ||
      !AppendHintInt32(&hints, "sender-pid", sender_pid) ||
      !dbus_message_iter_close_container(&args, &hints) ||
      !dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &expire_ms)) {
    dbus_message_unref(msg);
    LogFailOnce("dbus Notify hints failed");
    return;
  }

  DBusError err;
  dbus_error_init(&err);
  // Watch thread only — must not run on UI.
  DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
  dbus_message_unref(msg);
  if (!reply || dbus_error_is_set(&err)) {
    Log().warning << "Notify call failed: " << (err.message ? err.message : "unknown");
    if (dbus_error_is_set(&err)) {
      dbus_error_free(&err);
    }
    if (reply) {
      dbus_message_unref(reply);
    }
    return;
  }

  dbus_uint32_t new_id = 0;
  if (!dbus_message_get_args(reply, &err, DBUS_TYPE_UINT32, &new_id, DBUS_TYPE_INVALID)) {
    Log().warning << "Notify reply parse failed";
    if (dbus_error_is_set(&err)) {
      dbus_error_free(&err);
    }
    dbus_message_unref(reply);
    return;
  }
  dbus_message_unref(reply);

  if (!post.thread_id.empty() && new_id != 0) {
    std::lock_guard<std::mutex> lock(g_mu);
    const auto old = g_thread_to_id.find(post.thread_id);
    if (old != g_thread_to_id.end()) {
      g_id_to_thread.erase(old->second);
    }
    g_thread_to_id[post.thread_id] = new_id;
    g_id_to_thread[new_id] = post.thread_id;
  }
  Log().info << "Posted notification id=" << new_id << " thread=" << post.thread_id;
}

bool InitConnection() {
  dbus_threads_init_default();

  DBusError err;
  dbus_error_init(&err);
  DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
  if (!conn || dbus_error_is_set(&err)) {
    LogFailOnce(err.message ? err.message : "dbus session bus unavailable");
    if (dbus_error_is_set(&err)) {
      dbus_error_free(&err);
    }
    return false;
  }
  dbus_connection_set_exit_on_disconnect(conn, FALSE);
  if (!dbus_bus_register(conn, &err)) {
    LogFailOnce(err.message ? err.message : "dbus_bus_register failed");
    if (dbus_error_is_set(&err)) {
      dbus_error_free(&err);
    }
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    return false;
  }

  dbus_error_init(&err);
  dbus_bus_add_match(
      conn, "type='signal',interface='org.freedesktop.Notifications',member='ActionInvoked'", &err);
  if (dbus_error_is_set(&err)) {
    LogFailOnce(err.message ? err.message : "dbus add_match ActionInvoked failed");
    dbus_error_free(&err);
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    return false;
  }

  dbus_error_init(&err);
  dbus_bus_add_match(
      conn, "type='signal',interface='org.freedesktop.Notifications',member='ActivationToken'",
      &err);
  if (dbus_error_is_set(&err)) {
    dbus_error_free(&err);
  }

  dbus_error_init(&err);
  dbus_bus_add_match(
      conn, "type='signal',interface='org.freedesktop.Notifications',member='NotificationClosed'",
      &err);
  if (dbus_error_is_set(&err)) {
    dbus_error_free(&err);
  }

  if (!dbus_connection_add_filter(conn, FilterMessage, nullptr, nullptr)) {
    LogFailOnce("dbus add_filter failed");
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_conn = conn;
    g_init_ok = true;
  }
  Log().info << "Freedesktop notification watcher started";
  return true;
}

void WatchLoop() {
  if (!InitConnection()) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_pending_posts.clear();
    g_pending_closes.clear();
    g_pending_activations.clear();
    g_running.store(false, std::memory_order_release);
    return;
  }

  while (g_running.load(std::memory_order_acquire)) {
    DBusConnection* conn = nullptr;
    std::vector<PendingPost> posts;
    std::vector<uint32_t> closes;
    std::vector<std::string> activations;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      conn = g_conn;
      posts.swap(g_pending_posts);
      closes.swap(g_pending_closes);
      activations.swap(g_pending_activations);
    }
    if (!conn) {
      break;
    }

    // Single-threaded dbus ownership: pump, then method calls, then UI callbacks.
    dbus_connection_read_write(conn, posts.empty() && closes.empty() ? 100 : 0);
    while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS) {
    }

    for (uint32_t id : closes) {
      CloseNotificationId(conn, id);
    }
    for (const PendingPost& post : posts) {
      SendNotify(conn, post);
    }

    // Activations enqueued by filters during pump/Notify — take another snapshot.
    {
      std::lock_guard<std::mutex> lock(g_mu);
      if (!g_pending_activations.empty()) {
        activations.insert(activations.end(), g_pending_activations.begin(),
                           g_pending_activations.end());
        g_pending_activations.clear();
      }
    }
    for (const std::string& thread_id : activations) {
      DispatchDesktopNotificationActivation(thread_id);
    }
  }
}

void EnsureWatchStartedLocked() {
  if (g_init_started) {
    return;
  }
  g_init_started = true;
  g_running.store(true, std::memory_order_release);
  g_watch_thread = std::thread(WatchLoop);
}

} // namespace

void PostDesktopNotification(const std::string& title, const std::string& body,
                             const std::string& thread_id) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_init_started && !g_init_ok && !g_running.load(std::memory_order_acquire)) {
    // Init already failed on the watch thread.
    return;
  }
  EnsureWatchStartedLocked();
  PendingPost post;
  post.title = title;
  post.body = body;
  post.thread_id = thread_id;
  g_pending_posts.push_back(std::move(post));
}

void ClearDesktopNotification(const std::string& thread_id) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (thread_id.empty()) {
    return;
  }
  const auto it = g_thread_to_id.find(thread_id);
  if (it == g_thread_to_id.end()) {
    return;
  }
  const uint32_t id = it->second;
  g_id_to_thread.erase(id);
  g_thread_to_id.erase(it);
  g_pending_closes.push_back(id);
}

void ShutdownDesktopNotifications() {
  SetDesktopNotificationActivationHandler(nullptr);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_running.store(false, std::memory_order_release);
  }
  // Must join: an unjoined std::thread calls std::terminate in its destructor
  // ("terminate called without an active exception") on process exit.
  if (g_watch_thread.joinable()) {
    g_watch_thread.join();
  }
  std::lock_guard<std::mutex> lock(g_mu);
  g_thread_to_id.clear();
  g_id_to_thread.clear();
  g_pending_activations.clear();
  g_pending_closes.clear();
  g_pending_posts.clear();
  if (g_conn) {
    dbus_connection_close(g_conn);
    dbus_connection_unref(g_conn);
    g_conn = nullptr;
  }
  g_init_started = false;
  g_init_ok = false;
}

} // namespace pbr::desktop

#endif
