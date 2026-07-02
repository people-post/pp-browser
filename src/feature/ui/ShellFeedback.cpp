#include "feature/ui/ShellFeedback.h"

#include <algorithm>

namespace pbr {

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

} // namespace

void ShellFeedback::ShowToast(ShellState& state, const std::string& message, ToastDuration duration, float now_ms) {
  const ShellConfig config;
  ToastEntry entry;
  entry.id = NextToastId(state);
  entry.message = Rml::String(message.c_str());
  entry.expires_at_ms = now_ms + DurationMs(duration);
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

void ShellFeedback::ShowBanner(ShellState& state, const std::string& message) {
  state.banner_message = Rml::String(message.c_str());
}

void ShellFeedback::DismissBanner(ShellState& state) {
  state.banner_message = {};
}

void ShellFeedback::ShowAlert(ShellState& state, const std::string& title, const std::string& message,
                              std::function<void()> on_ok) {
  state.dialog.active = true;
  state.dialog.kind = OverlayKind::Alert;
  state.dialog.title = Rml::String(title.c_str());
  state.dialog.message = Rml::String(message.c_str());
  state.dialog.show_cancel = false;
  state.dialog.show_checkbox = false;
  state.dialog.checkbox_checked = false;
  state.dialog.on_result = [on_ok = std::move(on_ok)](bool ok, bool) {
    if (ok && on_ok) {
      on_ok();
    }
  };
}

void ShellFeedback::ShowConfirm(ShellState& state, const std::string& title, const std::string& message,
                                std::function<void(bool)> on_result) {
  state.dialog.active = true;
  state.dialog.kind = OverlayKind::Confirm;
  state.dialog.title = Rml::String(title.c_str());
  state.dialog.message = Rml::String(message.c_str());
  state.dialog.show_cancel = true;
  state.dialog.show_checkbox = false;
  state.dialog.checkbox_checked = false;
  state.dialog.on_result = [on_result = std::move(on_result)](bool ok, bool) {
    if (on_result) {
      on_result(ok);
    }
  };
}

void ShellFeedback::ShowConfirmWithCheckbox(ShellState& state, const std::string& title, const std::string& message,
                                            const std::string& checkbox_label, const bool checkbox_default,
                                            std::function<void(bool, bool)> on_result) {
  state.dialog.active = true;
  state.dialog.kind = OverlayKind::Confirm;
  state.dialog.title = Rml::String(title.c_str());
  state.dialog.message = Rml::String(message.c_str());
  state.dialog.show_cancel = true;
  state.dialog.show_checkbox = true;
  state.dialog.checkbox_label = Rml::String(checkbox_label.c_str());
  state.dialog.checkbox_checked = checkbox_default;
  state.dialog.on_result = std::move(on_result);
}

void ShellFeedback::DialogOk(ShellState& state) {
  if (!state.dialog.active) {
    return;
  }
  const bool checkbox_checked = state.dialog.checkbox_checked;
  auto callback = std::move(state.dialog.on_result);
  state.dialog = {};
  state.transient_active = !state.transient_stack.empty();
  if (callback) {
    callback(true, checkbox_checked);
  }
}

void ShellFeedback::DialogCancel(ShellState& state) {
  if (!state.dialog.active) {
    return;
  }
  auto callback = std::move(state.dialog.on_result);
  state.dialog = {};
  state.transient_active = !state.transient_stack.empty();
  if (callback) {
    callback(false, false);
  }
}

} // namespace pbr
