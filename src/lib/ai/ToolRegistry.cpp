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

/** The app's own tools: they act on in-process state (chats, settings), unlike MCP servers or web search. */
bool IsAppAction(const ToolMeta& meta) {
  return meta.provider == "messaging" || meta.provider == "settings";
}

/** One app action at a time, whichever AI session's turn asks for it. */
std::mutex& AppActionMutex() {
  static std::mutex mutex;
  return mutex;
}

/**
 * Runs `tool` on the UI thread and waits for it. A UI thread that has not started it within `timeout` gets it
 * cancelled: it fails here and never runs later. Once it has started, its result is awaited.
 */
Roe<std::string> RunOnUi(const ToolDescriptor& tool, const Object& arguments, const std::chrono::seconds timeout) {
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
  std::unique_lock lock(slot->mu);
  if (!slot->cv.wait_for(lock, timeout, [&] { return slot->started; })) {
    slot->abandoned = true;
    return Error("The app did not run " + tool.definition.name + " in time");
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

Roe<std::string> ToolRegistry::Execute(const std::string& name, const Object& arguments) const {
  for (const ToolDescriptor& tool : tools_) {
    if (tool.definition.name != name) {
      continue;
    }
    std::unique_lock<std::mutex> app_action;
    if (IsAppAction(tool.meta)) {
      app_action = std::unique_lock(AppActionMutex());
    }
    // Without a running runtime (unit tests of a tool) there is no UI loop to hand it to.
    if (tool.meta.ui_thread && AppRuntime::IsRunning() && !AppRuntime::CurrentlyOnUI()) {
      return RunOnUi(tool, arguments, kUiToolTimeout);
    }
    return tool.execute(arguments);
  }
  return Error("Unknown tool: " + name);
}

} // namespace pbr
