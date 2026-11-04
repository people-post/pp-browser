#include "lib/ai/ToolRegistry.h"

#include "common/runtime/AppRuntime.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string FormatMetaTag(const ToolMeta& meta) {
  std::string tag;
  if (!meta.domain.empty()) {
    tag = meta.domain;
  }
  if (!meta.risk.empty()) {
    if (!tag.empty()) {
      tag += ", ";
    }
    tag += meta.risk;
  } else if (meta.mutating) {
    if (!tag.empty()) {
      tag += ", ";
    }
    tag += "write";
  }
  return tag;
}

/** One app action at a time, whichever AI session's turn asks for it. */
std::timed_mutex& AppActionMutex() {
  static std::timed_mutex mutex;
  return mutex;
}

/** The turn was stopped, or the app is going away: stop waiting for the lock or the UI thread. */
bool Abandoned(const std::atomic<bool>* cancel) {
  return (cancel && cancel->load()) || AppRuntime::IsShuttingDown();
}

/**
 * Runs `tool` on the UI thread and waits for it. A UI thread that has not started it within `timeout` (or a turn
 * stopped meanwhile, or the app shutting down) gets it cancelled: it fails here and never runs later. Once it has
 * started, its result is awaited.
 */
Roe<std::string> RunOnUi(const ToolDescriptor& tool, const Object& arguments, const std::atomic<bool>* cancel) {
  struct Slot {
    std::mutex mu;
    std::condition_variable cv;
    bool started = false;
    bool abandoned = false;
    std::optional<Roe<std::string>> result;
  };
  auto slot = std::make_shared<Slot>();
  AppRuntime::PostUI([slot, execute = tool.execute, arguments]() {
    {
      std::lock_guard lock(slot->mu);
      if (slot->abandoned) {
        return;
      }
      slot->started = true;
    }
    Roe<std::string> result = execute(arguments);
    {
      std::lock_guard lock(slot->mu);
      slot->result.emplace(std::move(result));
    }
    slot->cv.notify_all();
  });
  const auto deadline = std::chrono::steady_clock::now() + ToolRegistry::kUiToolTimeout;
  std::unique_lock lock(slot->mu);
  while (!slot->started) {
    if (Abandoned(cancel)) {
      slot->abandoned = true;
      return Error("Stopped before the app ran " + tool.definition.name);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      slot->abandoned = true;
      return Error("The app did not run " + tool.definition.name + " in time");
    }
    slot->cv.wait_for(lock, ToolRegistry::kWaitSlice);
  }
  slot->cv.wait(lock, [&] { return slot->result.has_value(); });
  return std::move(*slot->result);
}

} // namespace

ToolRegistry::ToolRegistry() {
  redirectLogger("ToolRegistry");
}

void ToolRegistry::Register(ToolDescriptor tool) {
  for (const ToolDescriptor& existing : tools_) {
    if (existing.definition.name == tool.definition.name) {
      log().warning << "Replacing duplicate tool: " << tool.definition.name;
    }
  }
  tools_.push_back(std::move(tool));
}

void ToolRegistry::RegisterProvider(IToolProvider& provider) {
  const std::string provider_id = provider.Id();
  for (ToolDescriptor& tool : provider.ListTools()) {
    if (tool.meta.provider.empty()) {
      tool.meta.provider = provider_id;
    }
    bool exists = false;
    for (const ToolDescriptor& existing : tools_) {
      if (existing.definition.name == tool.definition.name) {
        log().warning << "Skipping duplicate tool from provider " << provider_id << ": "
                      << tool.definition.name;
        exists = true;
        break;
      }
    }
    if (exists) {
      continue;
    }
    tools_.push_back(std::move(tool));
  }
}

void ToolRegistry::Clear() {
  tools_.clear();
}

std::vector<ToolDefinition> ToolRegistry::Definitions() const {
  std::vector<ToolDefinition> out;
  out.reserve(tools_.size());
  for (const ToolDescriptor& tool : tools_) {
    out.push_back(tool.definition);
  }
  return out;
}

std::string ToolRegistry::SummaryForPrompt() const {
  std::ostringstream out;
  for (const ToolDescriptor& tool : tools_) {
    out << "- " << tool.definition.name;
    const std::string tag = FormatMetaTag(tool.meta);
    if (!tag.empty()) {
      out << " [" << tag << "]";
    }
    out << ": " << tool.definition.description << "\n";
  }
  return out.str();
}

Roe<std::string> ToolRegistry::Execute(const std::string& name, const Object& arguments,
                                       const std::atomic<bool>* cancel) const {
  for (const ToolDescriptor& tool : tools_) {
    if (tool.definition.name != name) {
      continue;
    }
    // Without a running runtime (unit tests of a tool) there is no UI loop and no other turn: run it here.
    if (!AppRuntime::IsRunning() || AppRuntime::CurrentlyOnUI()) {
      return tool.execute(arguments);
    }
    std::unique_lock<std::timed_mutex> app_action;
    if (tool.meta.app_state || tool.meta.ui_thread) {
      app_action = std::unique_lock(AppActionMutex(), std::defer_lock);
      while (!app_action.try_lock_for(kWaitSlice)) {
        if (Abandoned(cancel)) {
          return Error("Stopped while another app action ran");
        }
      }
    }
    if (tool.meta.ui_thread) {
      return RunOnUi(tool, arguments, cancel);
    }
    return tool.execute(arguments);
  }
  return Error("Unknown tool: " + name);
}

} // namespace pbr
