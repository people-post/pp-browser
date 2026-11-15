#pragma once

#include "feature/settings/SettingsCommands.h"
#include "feature/settings/SettingsPortsViews.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "common/Error.h"

#include <ui/base/Types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace ui {
class DataModelConstructor;
}

namespace pbr {

/**
 * Me → Network → My relays (multi-relay R3, MR006, MR007): the relays of ours, preferred first, each with its
 * registration and reachability, or *Identity changed — not used* (MyRelaysViewFor decides; this renders). On the main
 * device: Add relay… (an origin field, its problem shown under it; a relay seen before with another identity asks
 * first), Make preferred, Remove… (asks first), Accept new identity… (asks first); the org's origin's changed
 * identity on any device. Its rows are fields of the settings model; the ports are the controller's, read when used.
 * UI thread (the list is read and changed on a worker).
 */
class SettingsMyRelays {
public:
  struct Row {
    ui::String origin;
    ui::String label;
    ui::String state;
    ui::String state_kind;
    ui::String reach;
    bool reach_failed = false;
    bool preferred = false;
    bool can_make_preferred = false;
    bool can_remove = false;
    bool can_accept_identity = false;
  };

  SettingsMyRelays(const SettingsCommands& commands, const ShellFeedbackPorts& feedback,
                   std::function<void(const Error&)> report_failure);
  ~SettingsMyRelays();

  /** Its fields and events in the settings model. */
  void Bind(ui::DataModelConstructor& ctor);
  /** Reads the list again and shows it (and follows its changes from now on). */
  void Refresh();

private:
  void Show(Roe<MyRelaysView> listed);
  void Watch();
  void StartAdd();
  void CancelAdd();
  void Add();
  /** One Add on a worker; `accept_changed`: the person confirmed a relay seen before with another identity. */
  void AddNow(const std::string& typed, bool accept_changed);
  /** The relay seen before with another identity: ask, then Add it anyway. */
  void ConfirmChangedSinceSeen(const std::string& typed);
  void MakePreferred(int index);
  void Remove(int index);
  void AcceptIdentity(int index);
  void AcceptOrgIdentity();
  /** The accept confirm for `origin` (its new identity), then the change. */
  void ConfirmAccept(const std::string& origin, const std::string& label);
  /** Runs `change` for the relay `origin` names on a worker (none while one runs); then the list again and `done`. */
  void Change(const std::string& origin, std::function<Roe<void>(const std::string& origin)> change,
              const std::string& done);
  const MyRelayRowView* RowAt(int index) const;
  void DirtyAll();

  const SettingsCommands& commands_;
  const ShellFeedbackPorts& feedback_;
  std::function<void(const Error&)> report_failure_;

  MyRelaysView view_;
  std::vector<Row> rows_;
  bool shown_ = false;
  bool editable_ = false;
  bool can_add_ = false;
  /** The org's origin's host when its identity changed (MR007); empty otherwise. */
  ui::String org_identity_changed_;
  /** The origin field is open. */
  bool adding_ = false;
  /** A change is under way (the buttons wait). */
  bool busy_ = false;
  ui::String draft_;
  ui::String add_error_;
  bool watching_ = false;
  /** Bumped per read: only the latest read's answer is shown. */
  uint64_t refresh_count_ = 0;
  /** Cleared when this goes: a change notice after it does nothing. */
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace pbr
