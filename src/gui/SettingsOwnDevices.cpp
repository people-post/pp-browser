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
  ctor.BindEventCallback("remove_own_device", DataEvent<int>([this](const int index) { Remove(index); }));
  ctor.BindEventCallback("make_main_device", DataEvent<int>([this](const int index) { MakeMain(index); }));
}

void SettingsOwnDevices::Refresh() {
  if (!commands_.own_devices) {
    Show(Error("Messaging is not ready"));
    return;
  }
  const uint64_t count = ++refresh_count_;
  AppRuntime::PostWorkerNormal([this, count]() {
    auto listed = commands_.own_devices ? commands_.own_devices() : Roe<OwnDevicesView>(Error("Messaging is not ready"));
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
  DataModelHost& host = DataModelHost::Instance();
  host.Dirty("settings", "own_device_rows");
  host.Dirty("settings", "own_devices_shown");
  host.Dirty("settings", "own_devices_signed_out");
  host.Dirty("settings", "own_devices_not_main");
}

const OwnDeviceView* SettingsOwnDevices::ManageableAt(const int index) const {
  if (index < 0 || index >= static_cast<int>(view_.devices.size())) {
    return nullptr;
  }
  const OwnDeviceView& device = view_.devices[static_cast<size_t>(index)];
  return device.can_manage ? &device : nullptr;
}

void SettingsOwnDevices::Remove(const int index) {
  ConfirmChange(index, Tr("settings.security.devices.remove_confirm_title"),
                Tr("settings.security.devices.remove_confirm_message"),
                Tr("settings.security.devices.remove_confirm_ok"), commands_.remove_own_device,
                Tr("settings.security.devices.removed"));
}

void SettingsOwnDevices::MakeMain(const int index) {
  ConfirmChange(index, Tr("settings.security.devices.make_main_confirm_title"),
                Tr("settings.security.devices.make_main_confirm_message"),
                Tr("settings.security.devices.make_main_confirm_ok"), commands_.make_main_device,
                Tr("settings.security.devices.made_main"));
}

void SettingsOwnDevices::ConfirmChange(const int index, const std::string& title, const std::string& message,
                                       const std::string& ok_label,
                                       std::function<Roe<void>(const std::string& peer_id)> change,
                                       const std::string& done) {
  const OwnDeviceView* device = ManageableAt(index);
  if (!device || !feedback_.show_confirm) {
    return;
  }
  if (!change) {
    report_failure_(Error("Messaging is not ready"));
    return;
  }
  feedback_.show_confirm(
      title, message,
      [this, peer_id = device->peer_id, change = std::move(change), done](const bool ok) {
        if (!ok) {
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
