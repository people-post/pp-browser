#pragma once

#include "feature/settings/SettingsCommands.h"
#include "feature/settings/SettingsPortsViews.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "common/Error.h"

#include <ui/base/Types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace ui {
class DataModelConstructor;
}

namespace pbr {

/**
 * Me → Security → Your devices (M021 d4): the account's devices, the main one first. On the main device each
 * other device can be taken off the account or made the main one (each asks first); elsewhere the list only
 * shows. Its rows are fields of the settings model. The ports are the controller's, read when used.
 *
 * UI thread (the list is read and changed on a worker).
 */
class SettingsOwnDevices {
public:
  struct Row {
    ui::String peer_id;
    ui::String label;
    bool is_this_device = false;
    bool is_main = false;
    bool can_manage = false;
  };

  SettingsOwnDevices(const SettingsCommands& commands, const ShellFeedbackPorts& feedback,
                     std::function<void(const Error&)> report_failure);

  /** Its fields and events in the settings model. */
  void Bind(ui::DataModelConstructor& ctor);
  /** Reads the list again and shows it. */
  void Refresh();

private:
  void Show(Roe<OwnDevicesView> listed);
  void Remove(int index);
  void MakeMain(int index);
  /** Runs `change` for the device at `index` on a worker once confirmed; then the list again and `done`. */
  void ConfirmChange(int index, const std::string& title, const std::string& message, const std::string& ok_label,
                     std::function<Roe<void>(const std::string& peer_id)> change, const std::string& done);
  const OwnDeviceView* ManageableAt(int index) const;

  const SettingsCommands& commands_;
  const ShellFeedbackPorts& feedback_;
  std::function<void(const Error&)> report_failure_;

  OwnDevicesView view_;
  std::vector<Row> rows_;
  /** Whether the section shows (a list or a signed-out device; not while messaging is down). */
  bool shown_ = false;
  bool signed_out_ = false;
  /** Shown, and this device is not the main one: it neither adds nor removes devices. */
  bool not_main_ = false;
  /** Bumped per read: only the latest read's answer is shown. */
  uint64_t refresh_count_ = 0;
};

} // namespace pbr
