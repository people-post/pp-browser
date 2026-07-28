#pragma once

#include <string>

namespace pbr {

/** Snapshot of call overlay layers used to decide Dirty vs SyncLayout. */
struct CallChromeLayer {
  bool ring_active = false;
  bool in_call_active = false;
  std::string ring_call_id;
  std::string in_call_id;
  std::string in_call_subtitle;
  std::string ring_caller_label;
  std::string ring_media_label;
  std::string in_call_title;
};

enum class CallChromeUpdate {
  None,      // idle poll / no visible change
  DirtyOnly, // subtitle/labels while layer already shown
  Remount,   // layer appear / disappear / switch call_id (UI uses DirtyWindow + data-if; no SyncLayout)
};

/** Pure gate: remount only on layer identity change; never remount on timer alone. */
CallChromeUpdate ClassifyCallChromeUpdate(const CallChromeLayer& synced, const CallChromeLayer& next);

} // namespace pbr
