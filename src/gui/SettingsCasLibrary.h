#pragma once

#include "feature/settings/SettingsCommands.h"
#include "feature/settings/SettingsPortsViews.h"
#include "gui/UnlockEnsurePorts.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "common/Error.h"

#include <ui/base/Types.h>
#include <ui/data/DataModelHandle.h>

#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace ui {
class DataModelConstructor;
}

namespace pbr {

/**
 * Me → Storage → Library: the content this device keeps, by filter; sharing an item publicly (and
 * taking it back), copying a public item's tip, and fetching one from a peer by its tip. Its rows are
 * fields of the settings model. The ports are the controller's, read when used.
 *
 * UI thread.
 */
class SettingsCasLibrary {
public:
  struct Row {
    ui::String content_id_hex;
    ui::String title;
    ui::String detail;
    ui::String realm_label;
    ui::String pin_label;
    bool can_share_publicly = false;
    bool can_unpublish = false;
    bool can_copy_tip = false;
  };

  SettingsCasLibrary(const SettingsCommands& commands, const ShellFeedbackPorts& feedback,
                     const UnlockEnsurePorts& unlock, std::function<void(const Error&)> report_failure);

  /** Its fields and events in the settings model. */
  void Bind(ui::DataModelConstructor& ctor);
  /** Lists the items again and shows them. */
  void Refresh();

private:
  void List();
  void Show();
  void SetFilter(const std::string& filter);
  void ShareCasPublicly(int index);
  void Unpublish(int index);
  void CopyTip(int index);
  void FetchTip();
  void PerformShare(const std::string& content_id_hex);
  void PerformUnpublish(const std::string& content_id_hex);
  void PerformFetchTip(const std::string& tip, const std::string& peer_relay_user_id);
  const CasLibraryItemView* ItemAt(int index) const;

  const SettingsCommands& commands_;
  const ShellFeedbackPorts& feedback_;
  const UnlockEnsurePorts& unlock_;
  std::function<void(const Error&)> report_failure_;

  std::string filter_ = "all";
  std::vector<CasLibraryItemView> items_;
  std::vector<Row> rows_;
  ui::String empty_label_;
};

} // namespace pbr
