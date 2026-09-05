#include "gui/shell/ShellFeedback.h"

#include <algorithm>
#include <chrono>

namespace pbr {

ShellFeedbackChromePorts ShellFeedback::chrome_ports_;

void ShellFeedback::BindChromePorts(ShellFeedbackChromePorts ports) {
  chrome_ports_ = std::move(ports);
}

void ShellFeedback::SyncDialogChrome(const char* /*reason*/) {
  if (chrome_ports_.remount_dialog) {
    chrome_ports_.remount_dialog();
  }
  // RemountDialogChrome mounts dialog presence and Dirtys feedback bindings.
}

namespace {

int NextToastId(ShellState& state) {
  int max_id = 0;
  for (const ToastEntry& toast : state.toasts) {
    max_id = std::max(max_id, toast.id);
  }
  return max_id + 1;
}

float DurationMs(ToastDuration duration) {
  return duration == ToastDuration::Long ? 6000.f : 3000.f;
}

/** Default ShowToast now_ms is 0; treat that as "use wall clock" so callers that omit
 *  timing still expire correctly under power-save (idle frames can be seconds apart). */
float ResolveNowMs(float now_ms) {
  if (now_ms > 0.f) {
    return now_ms;
  }
  using clock = std::chrono::steady_clock;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch());
  return static_cast<float>(ms.count());
}

} // namespace

void ShellFeedback::ShowToast(ShellState& state, const std::string& message, ToastDuration duration, float now_ms) {
  const ShellConfig config;
  ToastEntry entry;
  entry.id = NextToastId(state);
  entry.message = Rml::String(message.c_str());
  entry.expires_at_ms = ResolveNowMs(now_ms) + DurationMs(duration);
  state.toasts.push_back(std::move(entry));
  while (state.toasts.size() > config.max_toasts) {
    state.toasts.erase(state.toasts.begin());
  }
}

void ShellFeedback::ExpireToasts(ShellState& state, float now_ms) {
  state.toasts.erase(std::remove_if(state.toasts.begin(), state.toasts.end(),
                                    [now_ms](const ToastEntry& toast) { return toast.expires_at_ms <= now_ms; }),
                     state.toasts.end());
}

double ShellFeedback::SecondsUntilNextToastExpiry(const ShellState& state, float now_ms) {
  if (state.toasts.empty()) {
    return -1.0;
  }
  float soonest_ms = state.toasts.front().expires_at_ms;
  for (const ToastEntry& toast : state.toasts) {
    soonest_ms = std::min(soonest_ms, toast.expires_at_ms);
  }
  const float remaining_ms = soonest_ms - now_ms;
  if (remaining_ms <= 0.f) {
    return 0.0;
  }
  return static_cast<double>(remaining_ms) / 1000.0;
}

void ShellFeedback::ShowBanner(ShellState& state, const std::string& message) {
  state.banner_message = Rml::String(message.c_str());
}

void ShellFeedback::DismissBanner(ShellState& state) {
  state.banner_message = {};
}

void ShellFeedback::ShowAlert(ShellState& state, const std::string& title, const std::string& message,
                              std::function<void()> on_ok, const std::string& ok_label) {
  state.dialog.active = true;
  state.dialog.kind = OverlayKind::Alert;
  state.dialog.title = Rml::String(title.c_str());
  state.dialog.message = Rml::String(message.c_str());
  state.dialog.ok_label = Rml::String(ok_label.c_str());
  state.dialog.show_cancel = false;
  state.dialog.show_checkbox = false;
  state.dialog.checkbox_checked = false;
  state.dialog.show_prompt = false;
  state.dialog.prompt_value = {};
  state.dialog.on_prompt_result = {};
  state.dialog.on_result = [on_ok = std::move(on_ok)](bool ok, bool) {
    if (ok && on_ok) {
      on_ok();
    }
  };
  SyncDialogChrome("dialog_open");
}

void ShellFeedback::ShowConfirm(ShellState& state, const std::string& title, const std::string& message,
                                std::function<void(bool)> on_result, const std::string& ok_label) {
  state.dialog.active = true;
  state.dialog.kind = OverlayKind::Confirm;
  state.dialog.title = Rml::String(title.c_str());
  state.dialog.message = Rml::String(message.c_str());
  state.dialog.ok_label = Rml::String(ok_label.c_str());
  state.dialog.show_cancel = true;
  state.dialog.show_checkbox = false;
  state.dialog.checkbox_checked = false;
  state.dialog.show_prompt = false;
  state.dialog.prompt_value = {};
  state.dialog.on_prompt_result = {};
  state.dialog.on_result = [on_result = std::move(on_result)](bool ok, bool) {
    if (on_result) {
      on_result(ok);
    }
  };
  SyncDialogChrome("dialog_open");
}

void ShellFeedback::ShowConfirmWithCheckbox(ShellState& state, const std::string& title, const std::string& message,
                                            const std::string& checkbox_label, const bool checkbox_default,
                                            std::function<void(bool, bool)> on_result) {
  state.dialog.active = true;
  state.dialog.kind = OverlayKind::Confirm;
  state.dialog.title = Rml::String(title.c_str());
  state.dialog.message = Rml::String(message.c_str());
  state.dialog.ok_label = {};
  state.dialog.show_cancel = true;
  state.dialog.show_checkbox = true;
  state.dialog.checkbox_label = Rml::String(checkbox_label.c_str());
  state.dialog.checkbox_checked = checkbox_default;
  state.dialog.show_prompt = false;
  state.dialog.prompt_value = {};
  state.dialog.on_prompt_result = {};
  state.dialog.on_result = std::move(on_result);
  SyncDialogChrome("dialog_open");
}

void ShellFeedback::ShowPrompt(ShellState& state, const std::string& title, const std::string& message,
                               const std::string& default_value,
                               std::function<void(bool, std::string)> on_result) {
  state.dialog.active = true;
  state.dialog.kind = OverlayKind::Confirm;
  state.dialog.title = Rml::String(title.c_str());
  state.dialog.message = Rml::String(message.c_str());
  state.dialog.ok_label = {};
  state.dialog.show_cancel = true;
  state.dialog.show_checkbox = false;
  state.dialog.show_prompt = true;
  state.dialog.prompt_value = Rml::String(default_value.c_str());
  state.dialog.on_result = {};
  state.dialog.on_prompt_result = std::move(on_result);
  SyncDialogChrome("dialog_open");
}

void ShellFeedback::DialogOk(ShellState& state) {
  if (!state.dialog.active) {
    return;
  }
  const bool checkbox_checked = state.dialog.checkbox_checked;
  const std::string prompt_value = state.dialog.prompt_value.c_str();
  const bool is_prompt = state.dialog.show_prompt;
  auto callback = std::move(state.dialog.on_result);
  auto prompt_callback = std::move(state.dialog.on_prompt_result);
  state.dialog = {};
  state.transient_active = !state.transient_stack.empty();
  // Run callbacks before remount so handlers can update shell state that the remount picks up.
  if (is_prompt) {
    if (prompt_callback) {
      prompt_callback(true, prompt_value);
    }
  } else if (callback) {
    callback(true, checkbox_checked);
  }
  SyncDialogChrome("dialog_close");
}

void ShellFeedback::DialogCancel(ShellState& state) {
  if (!state.dialog.active) {
    return;
  }
  const bool is_prompt = state.dialog.show_prompt;
  auto callback = std::move(state.dialog.on_result);
  auto prompt_callback = std::move(state.dialog.on_prompt_result);
  state.dialog = {};
  state.transient_active = !state.transient_stack.empty();
  if (is_prompt) {
    if (prompt_callback) {
      prompt_callback(false, {});
    }
  } else if (callback) {
    callback(false, false);
  }
  SyncDialogChrome("dialog_close");
}

} // namespace pbr
