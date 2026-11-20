#include "gui/SettingsRunLogsActions.h"

#include "common/chat/MessagingLimits.h"
#include "common/runtime/AppRuntime.h"
#include "feature/settings/RunLogPrefs.h"
#include "foundation/diagnostics/CrashDump.h"
#ifdef PP_CRASH_PROBE
#include "foundation/diagnostics/CrashProbe.h"
#endif
#include "foundation/diagnostics/LogFile.h"
#include "foundation/diagnostics/RunLogBundle.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/product/BuildInfo.h"
#include "foundation/product/DevTools.h"
#include "gui/UserFeedback.h"
#include "gui/shell/DataModelHost.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/ui/DataEvent.h"

#include <ui/data/DataModelHandle.h>

#include <ctime>
#include <utility>

namespace pbr {

namespace {

/** Under the direct (peer-to-peer) attachment size, with room to spare: the archive never takes the large-file path. */
constexpr std::uintmax_t kRunLogArchiveMaxBytes = kMaxDirectAttachmentBytes - 16 * 1024;

} // namespace

SettingsRunLogsActions::SettingsRunLogsActions(SettingsHost& host, const SettingsCommands& commands,
                                               const ShellFeedbackPorts& feedback)
    : host_(host), commands_(commands), feedback_(feedback) {}

void SettingsRunLogsActions::Bind(ui::DataModelConstructor& ctor) {
  Refresh();
  ctor.Bind("show_run_logs", &show_);
  ctor.Bind("run_logs_collect", &collect_);
  ctor.Bind("run_logs_collect_crash", &collect_crash_);
  ctor.Bind("run_logs_sendable", &sendable_);
  ctor.Bind("run_logs_send_disabled", &send_disabled_);
  ctor.BindEventCallback("toggle_collect_run_logs", DataEvent<>([this]() { ToggleCollect(); }));
  ctor.BindEventCallback("toggle_collect_crash_logs", DataEvent<>([this]() { ToggleCollectCrash(); }));
  ctor.BindEventCallback("send_run_logs", DataEvent<>([this]() { Send(); }));
  ctor.Bind("probe_enabled", &probe_enabled_);
  ctor.Bind("probe_status", &probe_status_);
  ctor.Bind("probe_card_status", &probe_card_status_);
  ctor.BindEventCallback("probe_update_card", DataEvent<>([this]() { ProbeUpdateCard(); }));
  ctor.BindEventCallback("probe_crash", DataEvent<>([this]() { ProbeCrash(0); }));
  ctor.BindEventCallback("probe_throw", DataEvent<>([this]() { ProbeCrash(1); }));
  ctor.BindEventCallback("probe_hang10", DataEvent<>([this]() { ProbeHang(10); }));
  ctor.BindEventCallback("probe_hang30", DataEvent<>([this]() { ProbeHang(30); }));
  ctor.BindEventCallback("probe_refresh", DataEvent<>([this]() { ProbeRefresh(); }));
}

void SettingsRunLogsActions::ProbeRefresh() {
#ifdef PP_CRASH_PROBE
  probe_enabled_ = true;
  const CrashProbe::Status status = CrashProbe::ReadStatus(DataDir());
  probe_status_ = "已收到 " + std::to_string(status.payloads) + " 份 · 最近一份：" +
                  (status.last_time.empty() ? std::string("还没有") : status.last_time);
  const int card_mode = CrashProbe::UpdateCardMode();
  probe_card_status_ = card_mode == 1 ? "现在：有链接（点一下 → 无链接）"
                       : card_mode == 2 ? "现在：无链接（点一下 → 关）"
                                        : "现在：关（点一下 → 有链接）";
  DataModelHost::Instance().Dirty("settings", "probe_card_status");
  DataModelHost::Instance().Dirty("settings", "probe_enabled");
  DataModelHost::Instance().Dirty("settings", "probe_status");
#endif
}

void SettingsRunLogsActions::ProbeUpdateCard() {
#ifdef PP_CRASH_PROBE
  CrashProbe::CycleUpdateCardMode();
  ProbeRefresh();
#endif
}

void SettingsRunLogsActions::ProbeAsk(const std::string& what, std::function<void()> action) {
  if (!feedback_.show_confirm) {
    return;
  }
  feedback_.show_confirm("探测：" + what, "确定？这会让 App 闪退或卡住。",
                         [alive = std::weak_ptr<bool>(alive_), action = std::move(action)](const bool ok) {
                           if (ok && alive.lock()) {
                             action();
                           }
                         },
                         "确定");
}

void SettingsRunLogsActions::ProbeCrash(const int kind) {
#ifdef PP_CRASH_PROBE
  ProbeAsk(kind == 1 ? "未捕获异常" : "制造一次闪退", [kind]() { CrashProbe::Crash(kind); });
#else
  (void)kind;
#endif
}

void SettingsRunLogsActions::ProbeHang(const int seconds) {
#ifdef PP_CRASH_PROBE
  ProbeAsk("卡住 " + std::to_string(seconds) + " 秒", [seconds]() { CrashProbe::Hang(seconds); });
#else
  (void)seconds;
#endif
}

std::string SettingsRunLogsActions::DataDir() const {
  // machine.json sits where start-up pruning reads it: the default data root, which a config data_dir override
  // does not move (the session's own data_dir only when the app recorded none).
  const std::string session = host_.Store().IsInitialized() ? host_.Store().Snapshot().data_dir : std::string();
  return RunLogPrefs::MachineDir(session);
}

std::string SettingsRunLogsActions::LogPath() const {
  // The file the app opened at start (it can sit outside the session's data_dir); else where it would be.
  if (std::string installed = LogFile::InstalledPath(); !installed.empty()) {
    return installed;
  }
  const std::string data_dir = DataDir();
  return data_dir.empty() ? std::string() : LogFile::DefaultPath(data_dir);
}

void SettingsRunLogsActions::Refresh() {
  // Opening the page again: the confirm is modal, so with no archive being made nobody can still be answering one. A
  // sheet that went away without calling back must not leave the row grey.
  if (!preparing_) {
    busy_ = false;
  }
  show_ = RunLogsAllowed(kUiDemoBuild) && !DataDir().empty();
  // Unreadable counts as off, as for pruning and packaging.
  collect_ = show_ && RunLogPrefs::ReadCollect(DataDir(), true).value_or(false);
  collect_crash_ = show_ && RunLogPrefs::ReadCollectCrash(DataDir(), true);
  sendable_ = show_ && RunLogBundle::Sendable(LogFile::FamilyFiles(LogPath()), collect_);
  send_disabled_ = busy_ || !sendable_;
  ProbeRefresh();
  Dirty();
}

void SettingsRunLogsActions::Dirty() {
  DataModelHost& model = DataModelHost::Instance();
  model.Dirty("settings", "show_run_logs");
  model.Dirty("settings", "run_logs_collect");
  model.Dirty("settings", "run_logs_collect_crash");
  model.Dirty("settings", "run_logs_sendable");
  model.Dirty("settings", "run_logs_send_disabled");
}

void SettingsRunLogsActions::SetBusy(const bool busy) {
  busy_ = busy;
  send_disabled_ = busy_ || !sendable_;
  Dirty();
}

void SettingsRunLogsActions::ToggleCollect() {
  if (!show_) {
    return;
  }
  if (auto set = RunLogPrefs::SetCollect(DataDir(), LogPath(), !collect_); !set) {
    host_.ReportFailure(set.error().WithUser(Tr("settings.run_logs.save_failed")));
  }
  Refresh();
}

void SettingsRunLogsActions::ToggleCollectCrash() {
  if (!show_) {
    return;
  }
  if (auto set = RunLogPrefs::SetCollectCrash(DataDir(), !collect_crash_); !set) {
    host_.ReportFailure(set.error().WithUser(Tr("settings.run_logs.save_failed")));
  }
  Refresh();
}

void SettingsRunLogsActions::Send() {
  if (busy_ || !show_) {
    return;
  }
  Refresh();
  if (!sendable_) {
    return;
  }
  SetBusy(true); // grey at once: a second tap makes no second archive
  preparing_ = true;
  UserFeedback::Note(Tr("settings.run_logs.preparing"));
  const std::string data_dir = DataDir();
  const std::string log_path = LogPath();
  std::weak_ptr<bool> alive = alive_;
  AppRuntime::PostWorkerNormal([this, alive, data_dir, log_path]() {
    // Packaging follows the same retention as pruning: unknown = off = this launch and the one before only.
    const bool collect = RunLogPrefs::ReadCollect(data_dir, true).value_or(false);
    // Crash records go along only while "Collect crash logs" is on, whatever is on disk.
    const FileCrashRecordSource crash_source(CrashDump::PendingPath(data_dir));
    const bool with_crash = RunLogPrefs::ReadCollectCrash(data_dir, true);
    // The archive opens with this build ("1.0.0 · internal/141 · a1b2c3d4"), so a report names the build it came from.
    Roe<RunLogArchive> archive =
        RunLogBundle::Build(log_path, LogFile::EffectiveRetention(collect), kRunLogArchiveMaxBytes, std::time(nullptr),
                            with_crash ? &crash_source : nullptr, BuildIdentity());
    AppRuntime::PostUI([this, alive, collect, archive = std::move(archive)]() mutable {
      if (alive.lock()) {
        preparing_ = false;
        OnPrepared(std::move(archive), collect);
      }
    });
  });
}

void SettingsRunLogsActions::OnPrepared(Roe<RunLogArchive> archive, const bool collect) {
  UserFeedback::DismissToast();
  if (!archive) {
    SetBusy(false);
    host_.ReportFailure(Error(archive.error().message).WithUser(Tr("settings.run_logs.prepare_failed")));
    return;
  }
  if (!feedback_.show_confirm) {
    SetBusy(false);
    host_.ReportFailure(Error("Run logs cannot be sent").WithUser(Tr("settings.run_logs.unavailable")));
    return;
  }
  const std::string size = FormatAttachmentByteSize(archive->bytes.size());
  std::string range;
  if (archive->first_time.empty()) {
    range = Tr(collect ? "settings.run_logs.confirm_size_on" : "settings.run_logs.confirm_size_off", {{"size", size}});
  } else {
    range = Tr(collect ? "settings.run_logs.confirm_range_on" : "settings.run_logs.confirm_range_off",
               {{"from", archive->first_time}, {"size", size}});
  }
  const std::string message =
      range + " " +
      Tr(archive->crash_included ? "settings.run_logs.confirm_contents_crash" : "settings.run_logs.confirm_contents") +
      " " + Tr("settings.run_logs.confirm_recipient");
  auto ready = std::make_shared<RunLogArchive>(std::move(*archive));
  std::weak_ptr<bool> alive = alive_;
  feedback_.show_confirm(
      Tr("settings.run_logs.confirm_title"), message,
      [this, alive, ready](const bool ok) {
        if (!alive.lock()) {
          return;
        }
        SetBusy(false);
        if (!ok) {
          return;
        }
        if (!commands_.send_run_logs) {
          host_.ReportFailure(Error("Run logs cannot be sent").WithUser(Tr("settings.run_logs.unavailable")));
          return;
        }
        OutgoingAttachment attachment;
        attachment.filename = ready->filename;
        attachment.mime = ready->mime;
        attachment.bytes = std::move(ready->bytes);
        commands_.send_run_logs(std::move(attachment));
      },
      Tr("settings.run_logs.pick"));
}

} // namespace pbr
