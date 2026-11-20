// Settings → Advanced: "Collect run logs" and "Send run logs" (DEV/PLAN-send-run-logs.md), driven through the real
// settings model on the real rows (the card cut from settings_section_advanced.rml), the session store on a temporary
// data directory with log files in it. What is checked: the switch is off on a fresh device and its words show; the
// row is grey only when the switch is off and the pp-browser logs hold this launch only; a tap greys it at once, so
// two taps make one archive and one confirm; the confirm says what is in it (two wordings, on and off); OK hands one
// .log.gz to the pick step, Cancel nothing; turning the switch off deletes the older logs; no file is written.
#include "common/runtime/AppRuntime.h"
#include "feature/settings/RunLogPrefs.h"
#include "feature/settings/SettingsCommands.h"
#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"
#include "foundation/diagnostics/LogFile.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/DeploymentProfile.h"
#include "gui/SettingsController.h"
#include "gui/UserFeedback.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/UiEditSession.h"

#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <zlib.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

namespace fs = std::filesystem;

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

class QuietSystem : public ui::SystemInterface {
public:
  double GetElapsedTime() override { return 0.0; }
};

std::string Gunzip(const std::vector<uint8_t>& bytes) {
  z_stream stream{};
  inflateInit2(&stream, 15 + 16);
  stream.next_in = const_cast<Bytef*>(bytes.data());
  stream.avail_in = static_cast<uInt>(bytes.size());
  std::string out;
  char buffer[65536];
  int result = Z_OK;
  do {
    stream.next_out = reinterpret_cast<Bytef*>(buffer);
    stream.avail_out = sizeof(buffer);
    result = inflate(&stream, Z_NO_FLUSH);
    out.append(buffer, sizeof(buffer) - stream.avail_out);
  } while (result == Z_OK);
  inflateEnd(&stream);
  return out;
}

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(fs::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

/** The two rows as the Advanced page has them, `{{i18n:…}}` put in as the view loader does. */
std::string RunLogsCard() {
  const std::string page = ReadAsset("views/settings_section_advanced.rml");
  const size_t start = page.find("<div class=\"group-card\" data-if=\"show_run_logs\">");
  const size_t end = page.find("<p class=\"text group-head\">{{i18n:settings.advanced.group.connection}}", start);
  if (start == std::string::npos || end == std::string::npos) {
    return {};
  }
  std::string card = page.substr(start, end - start);
  for (size_t at = card.find("{{i18n:"); at != std::string::npos; at = card.find("{{i18n:", at)) {
    const size_t close = card.find("}}", at);
    card.replace(at, close + 2 - at, Tr(card.substr(at + 7, close - at - 7)));
  }
  return card;
}

constexpr const char* kContext = "settings-run-logs-test";

class SettingsRunLogsTest : public testing::Test {
protected:
  void SetUp() override {
    while (UiEditSession::Instance().RemountBlocking()) {
      UiEditSession::Instance().EndRemount();
    }
    AppRuntime::Initialize();
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
    ui::SetRenderInterface(&render);
    ui::SetSystemInterface(&system);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext(kContext, {400, 800});
    ASSERT_NE(context, nullptr);

    root = fs::temp_directory_path() / ("pp_run_logs_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::remove_all(root);
    fs::create_directories(root / "profile");
    fs::create_directories(root / "logs");
    log_path = LogFile::DefaultPath(root.string());
    // This launch, and the other two families in the same directory (they never count for "anything to send").
    Write(log_path, "[2026-11-19 09:05:00.000] [INFO] this launch\n", 0);
    Write(LogFile::MetricsPath(log_path), "[2026-11-19 09:05:00.000] [INFO] [Metrics] event=x\n", 0);
    Write(LogFile::DiagPath(log_path), "[2026-11-19 09:05:00.000] [INFO] [Diag] event=y\n", 0);

    BootstrapResult bootstrap{};
    bootstrap.config = Config::DefaultAppConfig();
    bootstrap.config_path = (root / "config.json").string();
    // The session's data_dir is NOT the default data root (a config override): the switch still lives in `root`.
    fs::create_directories(root / "session");
    bootstrap.data_dir = (root / "session").string();
    RunLogPrefs::SetMachineDir(root.string());
    bootstrap.profile_data_dir = (root / "profile").string();
    bootstrap.profile_prefs = UserPreferences::DefaultProfile();
    store.SetUiDemo(false);
    store.Initialize(std::move(bootstrap));

    commands.session_store = [this]() -> SessionStore& { return store; };
    commands.send_run_logs = [this](OutgoingAttachment archive) { sent.push_back(std::move(archive)); };
    settings.BindCommands(commands);
    ports.show_toast = [this](const std::string& message, ToastDuration) { toasts.push_back(message); };
    ports.dismiss_toast = [] {};
    ports.show_confirm = [this](const std::string& title, const std::string& message,
                                std::function<void(bool)> on_result, const std::string& ok_label) {
      confirms.push_back({title, message, ok_label});
      answer = std::move(on_result);
    };
    settings.BindShellFeedback(ports);
    UserFeedback::BindPorts(ports);

    ASSERT_TRUE(settings.RegisterModel(context));
    const std::string card = RunLogsCard();
    ASSERT_FALSE(card.empty()) << "the rows are on the Advanced page";
    document = context->LoadDocumentFromMemory(
        "<rml><body><div data-model=\"settings\">"
        "<button id=\"toggle_adv\" data-event-click=\"toggle_advanced_settings()\">x</button>"
        "<button id=\"sel_adv\" data-event-click=\"select_section('advanced')\">x</button>" +
        card + "</div></body></rml>");
    ASSERT_NE(document, nullptr);
    document->Show();
    Click("toggle_adv"); // Help → "Show advanced settings": the Advanced page is listed
    Click("sel_adv");
  }
  void TearDown() override {
    RunLogPrefs::SetMachineDir("");
    AppRuntime::DrainWorkersThenUI();
    AppRuntime::RunUITasks();
    UserFeedback::BindPorts({});
    settings.BindShellFeedback({});
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext(kContext);
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetSystemInterface(nullptr);
    ui::SetRenderInterface(nullptr);
    AppRuntime::Shutdown();
    fs::remove_all(root);
  }

  static void Write(const std::string& path, const std::string& text, const int hours_old) {
    {
      std::ofstream out(path, std::ios::trunc);
      out << text;
    }
    fs::last_write_time(path,
                        fs::file_time_type::clock::now() - std::chrono::hours(hours_old) - std::chrono::minutes(1));
  }
  void PreviousLaunches(const int count) {
    for (int i = 1; i <= count; ++i) {
      Write(LogFile::RotatedPath(log_path, static_cast<size_t>(i)),
            "[2026-11-1" + std::to_string(8 - (i - 1) % 8) + " 08:00:00.000] [INFO] launch " + std::to_string(i) + "\n",
            i * 10);
    }
  }
  void Click(ui::Element* element) {
    ASSERT_NE(element, nullptr);
    element->DispatchEvent(ui::EventId::Click, {});
    context->Update();
  }
  void Click(const char* id) { Click(document->GetElementById(id)); }
  ui::Element* ByClass(const char* name) {
    ui::ElementList found;
    document->GetElementsByClassName(found, name);
    return found.empty() ? nullptr : found.front();
  }
  ui::Element* Switch() { return ByClass("settings-switch"); }
  ui::Element* Pill() { return ByClass("settings-pill-btn"); }
  ui::Element* NothingYet() {
    ui::ElementList subs;
    document->GetElementsByClassName(subs, "group-row-sub");
    for (ui::Element* sub : subs) {
      if (std::string(sub->GetInnerRML().c_str()) == Tr("settings.run_logs.nothing_yet")) {
        return sub;
      }
    }
    return nullptr;
  }
  bool Shown(ui::Element* element) {
    return element && element->GetComputedValues().display() != ui::Style::Display::None;
  }
  /**
   * The archive is made on a worker and comes back on the UI thread. DrainWorkersThenUI is a barrier behind the queued
   * tasks, not behind one that is still running on another worker thread, so with `want_confirms` the pump goes on
   * until that many confirms have been shown (a slow disk, as on Windows, made the archive land after one pass).
   */
  void Settle(const size_t want_confirms = 0) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
      AppRuntime::DrainWorkersThenUI();
      AppRuntime::RunUITasks();
      if (confirms.size() >= want_confirms) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    context->Update();
  }
  size_t PrunableFiles() { return LogFile::FamilyFiles(log_path).size(); }

  struct Confirm {
    std::string title;
    std::string message;
    std::string ok;
  };

  NullRender render;
  ShellFeedbackPorts ports;
  QuietSystem system;
  SessionStore store;
  SettingsCommands commands;
  SettingsController settings;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  bool initialized = false;
  fs::path root;
  std::string log_path;
  std::vector<std::string> toasts;
  std::vector<Confirm> confirms;
  std::function<void(bool)> answer;
  std::vector<OutgoingAttachment> sent;
};

TEST_F(SettingsRunLogsTest, OffOnAFreshDeviceWithItsWordsAndNothingToSendYet) {
  ASSERT_TRUE(RunLogsAllowed(/*ui_demo=*/false));
  const std::string text = document->GetInnerRML().c_str();
  EXPECT_NE(text.find("收集运行日志"), std::string::npos);
  EXPECT_NE(text.find("发送日志"), std::string::npos);
  EXPECT_NE(text.find(Tr("settings.run_logs.collect.help")), std::string::npos);
  ASSERT_NE(Switch(), nullptr);
  EXPECT_FALSE(Switch()->IsClassSet("settings-switch--on")) << "default off";
  EXPECT_TRUE(Shown(NothingYet())) << "off, and this launch is the only pp-browser log";
  EXPECT_TRUE(Pill()->HasAttribute("disabled"));
  Click(Pill());
  Settle();
  EXPECT_TRUE(confirms.empty()) << "nothing to send: no archive, no confirm";
}

TEST_F(SettingsRunLogsTest, ThePreviousLaunchMakesItSendableWithTheSwitchOff) {
  PreviousLaunches(1);
  Click("sel_adv"); // the page opened again reads the directory again
  EXPECT_FALSE(Shown(NothingYet()));
  EXPECT_FALSE(Pill()->HasAttribute("disabled"));
}

TEST_F(SettingsRunLogsTest, TheSwitchOnIsAlwaysSendableAndStoredForTheDevice) {
  Click(Switch());
  EXPECT_TRUE(Switch()->IsClassSet("settings-switch--on"));
  auto machine = UserPreferences::LoadMachine(root.string());
  ASSERT_TRUE(machine);
  EXPECT_TRUE(machine->collect_run_logs) << "machine.json, not the profile's preferences";
  EXPECT_FALSE(Shown(NothingYet()));
  EXPECT_FALSE(Pill()->HasAttribute("disabled"));
}

TEST_F(SettingsRunLogsTest, TheSwitchIsStoredInTheDefaultDataRootNotTheSessionsOverride) {
  Click(Switch());
  EXPECT_TRUE(fs::exists(root / "machine.json"));
  EXPECT_FALSE(fs::exists(root / "session" / "machine.json")) << "no stray file under the session's data_dir";
  // What start-up pruning reads next launch is what the switch wrote.
  EXPECT_EQ(RunLogPrefs::ReadCollect(RunLogPrefs::MachineDir(""), true), std::optional<bool>(true));
  EXPECT_EQ(RunLogPrefs::ReadCollect((root / "session").string(), true), std::optional<bool>(false))
      << "the other root never had the switch";
}

TEST_F(SettingsRunLogsTest, TwoTapsMakeOneArchiveAndOneConfirmThatSaysWhatIsInIt) {
  Click(Switch());
  PreviousLaunches(2);
  Click(Pill());
  EXPECT_TRUE(Pill()->HasAttribute("disabled")) << "grey at once";
  EXPECT_EQ(toasts.back(), Tr("settings.run_logs.preparing"));
  Click(Pill());
  Settle(1);
  ASSERT_EQ(confirms.size(), 1u);
  EXPECT_EQ(confirms[0].title, "发送日志");
  EXPECT_EQ(confirms[0].ok, "选择联系人");
  EXPECT_EQ(confirms[0].message.rfind("从 11-17 08:00 到现在的运行日志，约 ", 0), 0u) << confirms[0].message;
  EXPECT_NE(confirms[0].message.find(Tr("settings.run_logs.confirm_contents")), std::string::npos);
  EXPECT_NE(confirms[0].message.find(Tr("settings.run_logs.confirm_recipient")), std::string::npos);
  EXPECT_TRUE(Pill()->HasAttribute("disabled")) << "still grey while the confirm is up";

  answer(true);
  context->Update();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].mime, "application/gzip");
  EXPECT_EQ(sent[0].filename.rfind("pp-run-logs-", 0), 0u);
  ASSERT_GE(sent[0].bytes.size(), 2u);
  EXPECT_EQ(sent[0].bytes[0], 0x1f);
  EXPECT_EQ(sent[0].bytes[1], 0x8b);
  EXPECT_FALSE(Pill()->HasAttribute("disabled"));
}

TEST_F(SettingsRunLogsTest, AConfirmThatWentAwayWithoutAnswerLeavesTheRowUsableOnceThePageOpensAgain) {
  Click(Switch());
  PreviousLaunches(1);
  Click(Pill());
  Settle(1);
  ASSERT_EQ(confirms.size(), 1u);
  EXPECT_TRUE(Pill()->HasAttribute("disabled")) << "the confirm is up";
  // The sheet is torn down by something other than its buttons: no callback ever comes.
  Click("sel_adv"); // the Advanced page opened again
  EXPECT_FALSE(Pill()->HasAttribute("disabled"));
  Click(Pill());
  Settle(2);
  EXPECT_EQ(confirms.size(), 2u) << "a new tap makes a new confirm";
  EXPECT_TRUE(sent.empty());
}

TEST_F(SettingsRunLogsTest, OpeningThePageAgainWhileTheArchiveIsBeingMadeStaysGrey) {
  Click(Switch());
  PreviousLaunches(1);
  Click(Pill());
  Click("sel_adv"); // before the worker came back
  EXPECT_TRUE(Pill()->HasAttribute("disabled"));
  Settle(1);
  EXPECT_EQ(confirms.size(), 1u);
}

TEST_F(SettingsRunLogsTest, TheCrashSwitchIsOffByDefaultStoredForTheDeviceAndSitsBetweenCollectAndSend) {
  ui::ElementList switches;
  document->GetElementsByClassName(switches, "settings-switch");
  ASSERT_EQ(switches.size(), 2u) << "collect run logs, collect crash logs";
  ui::Element* crash = switches[1];
  EXPECT_FALSE(crash->IsClassSet("settings-switch--on")) << "default off";
  const std::string text = document->GetInnerRML().c_str();
  const size_t collect = text.find("收集运行日志");
  const size_t crash_row = text.find("收集闪退日志");
  const size_t send = text.find("发送日志");
  ASSERT_NE(crash_row, std::string::npos);
  EXPECT_LT(collect, crash_row);
  EXPECT_LT(crash_row, send);
  EXPECT_NE(text.find("只收集 App 的运行记录，用于排查和修复问题。"), std::string::npos);
  Click(crash);
  EXPECT_TRUE(crash->IsClassSet("settings-switch--on"));
  auto machine = UserPreferences::LoadMachine(root.string());
  ASSERT_TRUE(machine);
  EXPECT_TRUE(machine->collect_crash_logs);
  EXPECT_FALSE(machine->collect_run_logs) << "its own switch";
  EXPECT_FALSE(fs::exists(root / "session" / "machine.json"));
}

TEST_F(SettingsRunLogsTest, CrashRecordsGoOnlyWhileTheCrashSwitchIsOnWhateverIsOnDisk) {
  Click(Switch());
  PreviousLaunches(1);
  fs::create_directories(root / "diagnostics");
  Write((root / "diagnostics" / "crash_pending.txt").string(), "pp-browser crash dump\nreason=signal 11\n", 0);

  Click(Pill()); // crash switch off: the record is on disk but stays out
  Settle(1);
  ASSERT_EQ(confirms.size(), 1u);
  EXPECT_NE(confirms[0].message.find(Tr("settings.run_logs.confirm_contents")), std::string::npos);
  EXPECT_EQ(confirms[0].message.find("闪退记录"), std::string::npos);
  answer(true);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(Gunzip(sent[0].bytes).find("signal 11"), std::string::npos) << "no crash record in the archive";

  ui::ElementList switches;
  document->GetElementsByClassName(switches, "settings-switch");
  Click(switches[1]); // crash switch on
  Click(Pill());
  Settle(2);
  ASSERT_EQ(confirms.size(), 2u);
  EXPECT_NE(confirms[1].message.find("闪退记录"), std::string::npos) << confirms[1].message;
  EXPECT_EQ(confirms[1].message.find(Tr("settings.run_logs.confirm_contents")), std::string::npos);
  answer(true);
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_NE(Gunzip(sent[1].bytes).find("reason=signal 11"), std::string::npos);

  fs::remove(root / "diagnostics" / "crash_pending.txt"); // switch on, nothing on disk: no mention
  Click(Pill());
  Settle(3);
  ASSERT_EQ(confirms.size(), 3u);
  EXPECT_EQ(confirms[2].message.find("闪退记录"), std::string::npos);
}

TEST_F(SettingsRunLogsTest, OffSaysCollectingIsOffAndCancelSendsNothing) {
  PreviousLaunches(3); // only the newest of them is packaged
  Click("sel_adv");
  Click(Pill());
  Settle(1);
  ASSERT_EQ(confirms.size(), 1u);
  EXPECT_EQ(confirms[0].message.rfind("收集未打开，只包含这一次和上一次启动的记录，从 11-18 08:00 到现在，约 ", 0), 0u)
      << confirms[0].message;
  answer(false);
  context->Update();
  EXPECT_TRUE(sent.empty());
  EXPECT_FALSE(Pill()->HasAttribute("disabled"));
}

TEST_F(SettingsRunLogsTest, TurningTheSwitchOffDeletesTheOlderLogsAtOnce) {
  Click(Switch());
  PreviousLaunches(4);
  EXPECT_EQ(PrunableFiles(), 5u);
  Click(Switch());
  EXPECT_FALSE(Switch()->IsClassSet("settings-switch--on"));
  EXPECT_EQ(PrunableFiles(), 2u) << "this launch and the one before";
}

TEST_F(SettingsRunLogsTest, NoArchiveFileIsLeftOnDisk) {
  Click(Switch());
  PreviousLaunches(1);
  Click(Pill());
  Settle(1);
  ASSERT_EQ(confirms.size(), 1u);
  answer(true);
  for (const fs::path& dir : {root, fs::temp_directory_path()}) {
    for (const auto& entry : fs::directory_iterator(dir)) {
      EXPECT_EQ(entry.path().filename().string().rfind("pp-run-logs-", 0), std::string::npos) << entry.path();
    }
  }
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    EXPECT_EQ(entry.path().filename().string().rfind("pp-run-logs-", 0), std::string::npos) << entry.path();
  }
}

// The gate (RunLogsAllowed) closed: the rows are not shown (both sit in the one data-if) and the switch counts as off.
TEST(SettingsRunLogsGateTest, AClosedGateHidesBothRows) {
  EXPECT_FALSE(RunLogsAllowed(/*ui_demo=*/true));
  const std::string card = ReadAsset("views/settings_section_advanced.rml");
  const std::string gate_attr = "data-if=\"show_run_logs\"";
  const size_t gate = card.find(gate_attr);
  ASSERT_NE(gate, std::string::npos);
  EXPECT_LT(gate, card.find("toggle_collect_run_logs()"));
  EXPECT_LT(gate, card.find("send_run_logs()"));
  EXPECT_EQ(card.find("show_run_logs", gate + gate_attr.size()), std::string::npos) << "one gate for the two rows";
}

} // namespace
} // namespace pbr
