#include "gui/SettingsOwnDevices.h"

#include "foundation/i18n/LocalizationService.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "gui/UserFeedback.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/DataEvent.h"

#include <ui/data/DataModelHandle.h>

#include <utility>

namespace pbr {

SettingsOwnDevices::SettingsOwnDevices(const SettingsCommands& commands, const ShellFeedbackPorts& feedback,
                                       std::function<void(const Error&)> report_failure)
    : commands_(commands), feedback_(feedback), report_failure_(std::move(report_failure)) {}

void SettingsOwnDevices::Bind(ui::DataModelConstructor& ctor) {
  if (auto row = ctor.RegisterStruct<Row>()) {
    row.RegisterMember("peer_id", &Row::peer_id);
    row.RegisterMember("label", &Row::label);
    row.RegisterMember("is_this_device", &Row::is_this_device);
    row.RegisterMember("is_main", &Row::is_main);
    row.RegisterMember("can_manage", &Row::can_manage);
  }
  ctor.RegisterArray<std::vector<Row>>();
  ctor.Bind("own_device_rows", &rows_);
  ctor.Bind("own_devices_shown", &shown_);
  ctor.Bind("own_devices_signed_out", &signed_out_);
  ctor.Bind("own_devices_not_main", &not_main_);
  ctor.Bind("own_has_other_devices", &has_others_);
  ctor.BindEventCallback("remove_own_device", DataEvent<int>([this](const int index) { Remove(index); }));
  ctor.BindEventCallback("make_main_device", DataEvent<int>([this](const int index) { MakeMain(index); }));
  ctor.BindEventCallback("open_own_device_actions",
                         [this](ui::DataModelHandle /*model*/, ui::Event& ev, const ui::VariantList& args) {
                           if (const auto index = EventArgAsInt(args, 0)) {
                             ShowDeviceActions(ev, *index);
                           }
                         });
}

void SettingsOwnDevices::Refresh() {
  if (!commands_.own_devices) {
    Show(Error("Messaging is not ready"));
    return;
  }
  const uint64_t count = ++refresh_count_;
  AppRuntime::PostWorkerNormal([this, count]() {
    auto listed =
        commands_.own_devices ? commands_.own_devices() : Roe<OwnDevicesView>(Error("Messaging is not ready"));
    AppRuntime::PostUI([this, count, listed = std::move(listed)]() mutable {
      if (count == refresh_count_) {
        Show(std::move(listed));
      }
    });
  });
}

void SettingsOwnDevices::Show(Roe<OwnDevicesView> listed) {
  // Messaging not up (or no account yet): nothing to show, and nothing to report — Security still opens.
  view_ = listed ? std::move(*listed) : OwnDevicesView{};
  rows_.clear();
  rows_.reserve(view_.devices.size());
  for (const OwnDeviceView& device : view_.devices) {
    rows_.push_back({.peer_id = device.peer_id.c_str(),
                     .label = device.label.c_str(),
                     .is_this_device = device.is_this_device,
                     .is_main = device.is_main,
                     .can_manage = device.can_manage});
  }
  shown_ = !rows_.empty() || view_.signed_out;
  signed_out_ = view_.signed_out;
  not_main_ = shown_ && !view_.this_is_main;
  has_others_ = false;
  for (const OwnDeviceView& device : view_.devices) {
    has_others_ = has_others_ || !device.is_this_device;
  }
  DataModelHost& host = DataModelHost::Instance();
  host.Dirty("settings", "own_device_rows");
  host.Dirty("settings", "own_devices_shown");
  host.Dirty("settings", "own_devices_signed_out");
  host.Dirty("settings", "own_devices_not_main");
  host.Dirty("settings", "own_has_other_devices");
}

const OwnDeviceView* SettingsOwnDevices::ManageableAt(const int index) const {
  if (index < 0 || index >= static_cast<int>(view_.devices.size())) {
    return nullptr;
  }
  const OwnDeviceView& device = view_.devices[static_cast<size_t>(index)];
  return device.can_manage ? &device : nullptr;
}

const OwnDeviceView* SettingsOwnDevices::ManageableById(const std::string& peer_id) const {
  for (const OwnDeviceView& device : view_.devices) {
    if (device.peer_id == peer_id) {
      return device.can_manage ? &device : nullptr;
    }
  }
  return nullptr;
}

void SettingsOwnDevices::Remove(const int index) {
  if (const OwnDeviceView* device = ManageableAt(index)) {
    RemoveById(device->peer_id);
  }
}

void SettingsOwnDevices::MakeMain(const int index) {
  if (const OwnDeviceView* device = ManageableAt(index)) {
    MakeMainById(device->peer_id);
  }
}

void SettingsOwnDevices::RemoveById(const std::string& peer_id) {
  ConfirmChange(peer_id, Tr("settings.security.devices.remove_confirm_title"),
                "settings.security.devices.remove_confirm_message", Tr("settings.security.devices.remove_confirm_ok"),
                commands_.remove_own_device, Tr("settings.security.devices.removed"), true);
}

void SettingsOwnDevices::MakeMainById(const std::string& peer_id) {
  ConfirmChange(peer_id, Tr("settings.security.devices.make_main_confirm_title"),
                "settings.security.devices.make_main_confirm_message",
                Tr("settings.security.devices.make_main_confirm_ok"), commands_.make_main_device,
                Tr("settings.security.devices.made_main"));
}

std::vector<ContextMenuAction> SettingsOwnDevices::DeviceActions(const int index) {
  std::vector<ContextMenuAction> actions;
  const OwnDeviceView* device = ManageableAt(index);
  if (!device) {
    return actions;
  }
  // The sheet acts on the device it was opened for, not on a list position the list may have left by then.
  const std::string peer_id = device->peer_id;
  actions.push_back({.id = "own_device_make_main",
                     .label = Tr("settings.security.devices.make_main"),
                     .enabled = {},
                     .run = [this, peer_id]() { MakeMainById(peer_id); }});
  ContextMenuAction remove{.id = "own_device_remove",
                           .label = Tr("settings.security.devices.remove"),
                           .enabled = {},
                           .run = [this, peer_id]() { RemoveById(peer_id); }};
  remove.danger = true;
  actions.push_back(std::move(remove));
  ContextMenuAction cancel{"own_device_cancel", Tr("common.cancel"), nullptr, []() {}};
  cancel.cancel = true;
  actions.push_back(std::move(cancel));
  return actions;
}

void SettingsOwnDevices::ShowDeviceActions(ui::Event& ev, const int index) {
  std::vector<ContextMenuAction> actions = DeviceActions(index);
  if (actions.empty()) {
    return;
  }
  ContextMenuHost::Instance().ShowActions(MenuPositionBelowEvent(ev), std::move(actions), {},
                                          view_.devices[static_cast<size_t>(index)].label);
}

void SettingsOwnDevices::ConfirmChange(const std::string& peer_id, const std::string& title, const char* message_key,
                                       const std::string& ok_label,
                                       std::function<Roe<void>(const std::string& peer_id)> change,
                                       const std::string& done, const bool destructive) {
  const OwnDeviceView* device = ManageableById(peer_id);
  if (!device) {
    UserFeedback::Note(Tr("settings.security.devices.list_changed"));
    return;
  }
  if (!feedback_.show_confirm) {
    return;
  }
  if (!change) {
    report_failure_(Error("Messaging is not ready"));
    return;
  }
  const std::string message = Tr(message_key, {{"device", device->label}});
  const auto& show_confirm =
      destructive && feedback_.show_confirm_destructive ? feedback_.show_confirm_destructive : feedback_.show_confirm;
  show_confirm(
      title, message,
      [this, peer_id, change = std::move(change), done](const bool ok) {
        if (!ok) {
          return;
        }
        // The list may have changed while the dialog was open: look the device up again.
        if (!ManageableById(peer_id)) {
          UserFeedback::Note(Tr("settings.security.devices.list_changed"));
          return;
        }
        AppRuntime::PostWorkerNormal([this, peer_id, change, done]() {
          Roe<void> changed = change(peer_id);
          AppRuntime::PostUI([this, changed = std::move(changed), done]() {
            Refresh();
            if (!changed) {
              report_failure_(changed.error());
              return;
            }
            UserFeedback::Ok(done);
          });
        });
      },
      ok_label);
}

} // namespace pbr
