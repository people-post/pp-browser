#pragma once

#include "feature/settings/SettingsCommands.h"
#include "gui/SettingsHost.h"
#include "gui/shell/ShellFeedbackPorts.h"

#include <memory>
#include <string>

namespace ui {
class DataModelConstructor;
}

namespace pbr {

struct RunLogArchive;

/**
 * Settings → Advanced: "Collect run logs" (a device-wide switch, default off) and "Send run logs"
 * (DEV/PLAN-send-run-logs.md, a test-phase feature behind RunLogsAllowed). Sending: the row goes grey at once, the
 * archive is made off the UI thread ("Preparing…"), a confirm says what is in it and its real time range, then the
 * New chat page picks the contact it goes to (SettingsCommands::send_run_logs). Nothing is ever sent by itself.
 *
 * UI thread.
 */
class SettingsRunLogsActions {
public:
  SettingsRunLogsActions(SettingsHost& host, const SettingsCommands& commands, const ShellFeedbackPorts& feedback);
  SettingsRunLogsActions(const SettingsRunLogsActions&) = delete;
  SettingsRunLogsActions& operator=(const SettingsRunLogsActions&) = delete;

  /** Its fields and events in the settings model. */
  void Bind(ui::DataModelConstructor& ctor);
  /** The switch and whether there is anything to send, read again (on opening Advanced). */
  void Refresh();
  /** The switch's event: stores the other value; off deletes the older logs at once. */
  void ToggleCollect();
  /** The row's event: prepares the archive and asks first; a tap while one is being prepared does nothing. */
  void Send();

  bool Shown() const { return show_; }
  bool Collect() const { return collect_; }
  bool Sendable() const { return sendable_; }
  bool Busy() const { return busy_; }

private:
  void OnPrepared(Roe<RunLogArchive> archive, bool collect);
  void SetBusy(bool busy);
  void Dirty();
  std::string DataDir() const;
  std::string LogPath() const;

  SettingsHost& host_;
  const SettingsCommands& commands_;
  const ShellFeedbackPorts& feedback_;
  /** Work posted to a worker checks this before it comes back (the object may be gone by then). */
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  bool show_ = false;
  bool collect_ = false;
  bool sendable_ = false;
  bool busy_ = false;
  /** The archive is being made on a worker: busy_ stays up until it comes back. */
  bool preparing_ = false;
  bool send_disabled_ = true;
};

} // namespace pbr
