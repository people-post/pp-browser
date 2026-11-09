// The AI Home's animated mascot (Kenneth 2026-11-09): one look pref for the floating dot and the Home logo, "auto"
// changing it with the weekday; a tap only animates; the same figure and keyframes as the dot.
#include "gui/shell/HomeMascotModel.h"
#include "gui/shell/MascotFigure.h"
#include "gui/shell/MascotLook.h"
#include "gui/shell/ShellHomeMascot.h"

#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

TEST(MascotLookTest, EachWeekdayHasItsLook) {
  // 2026-11-09 is a Monday.
  EXPECT_EQ(WeekdayOf(2026, 11, 9), 1);
  const char expected[7] = {'a', 'b', 'c', 'd', 'e', 'f', 'g'}; // Mon … Sun
  for (int i = 0; i < 7; ++i) {
    const int day = 9 + i;
    const int weekday = WeekdayOf(2026, 11, day);
    EXPECT_EQ(LookForDate(2026, 11, day, weekday), expected[i]) << "Nov " << day;
  }
  EXPECT_EQ(WeekdayOf(2024, 2, 29), 4) << "a leap day (Thursday)";
  EXPECT_EQ(WeekdayOf(2000, 1, 1), 6);
}

TEST(MascotLookTest, ADateOverrideBeatsTheWeekdayAndTheShippedTableIsEmpty) {
  EXPECT_TRUE(HolidayLooks().empty());
  const MascotDayLook table[] = {{12, 24, 26, 'g'}, {1, 1, 1, 'e'}};
  const std::span<const MascotDayLook> overrides(table);
  // Dec 25 2026 is a Friday (e); the table says g. Dec 27 is outside the range: the weekday (Sunday, g) shows.
  EXPECT_EQ(LookForDate(2026, 12, 25, WeekdayOf(2026, 12, 25)), 'e');
  EXPECT_EQ(LookForDate(2026, 12, 24, WeekdayOf(2026, 12, 24), overrides), 'g');
  EXPECT_EQ(LookForDate(2026, 12, 26, WeekdayOf(2026, 12, 26), overrides), 'g');
  EXPECT_EQ(LookForDate(2026, 12, 28, WeekdayOf(2026, 12, 28), overrides), 'a') << "Monday again";
  EXPECT_EQ(LookForDate(2027, 1, 1, WeekdayOf(2027, 1, 1), overrides), 'e');
}

TEST(MascotLookTest, WeekendLooksBorrowPartsUntilTheirOwnLand) {
  EXPECT_EQ(PartsFor('e'), 'b');
  EXPECT_EQ(PartsFor('f'), 'a');
  EXPECT_EQ(PartsFor('g'), 'c');
  for (const char look : {'a', 'b', 'c', 'd'}) {
    EXPECT_EQ(PartsFor(look), look);
  }
}

TEST(MascotLookTest, AutoFollowsTheDateAndAFixedLookStays) {
  // The floating dot and the Home mascot resolve their look with this.
  EXPECT_EQ(ResolveLook("auto", 2026, 11, 9), 'a');  // Monday
  EXPECT_EQ(ResolveLook("auto", 2026, 11, 12), 'd'); // Thursday
  EXPECT_EQ(ResolveLook("auto", 2026, 11, 15), 'g'); // Sunday
  for (int day = 9; day < 16; ++day) {
    EXPECT_EQ(ResolveLook("c", 2026, 11, day), 'c') << "a picked look does not change with the day";
    EXPECT_EQ(ResolveLook("a", 2026, 11, day), 'a');
  }
  EXPECT_EQ(ResolveLook("z", 2026, 11, 10), 'b') << "an unknown pref is auto";
  const MascotDayLook table[] = {{11, 10, 10, 'd'}};
  EXPECT_EQ(ResolveLook("auto", 2026, 11, 10, table), 'd') << "auto consults the date overrides";
  EXPECT_EQ(ResolveLook("a", 2026, 11, 10, table), 'a') << "a picked look ignores them";
  EXPECT_TRUE(IsLookPref("auto"));
  EXPECT_FALSE(IsLookPref("e"));
  const char today = ResolveLookToday("auto");
  EXPECT_TRUE(today >= 'a' && today <= 'g');
  EXPECT_EQ(ResolveLookToday("d"), 'd');
}

TEST(HomeMascotModelTest, ItIsStillUntilTappedThenPokesAndPlaysOneRandomActionAndStops) {
  HomeMascotModel model;
  EXPECT_EQ(model.Idle(), AiDotIdle::None);
  EXPECT_FALSE(model.MsUntilNext(0).has_value()) << "nothing runs between taps";
  EXPECT_FALSE(model.Advance(100000));

  ASSERT_TRUE(model.Tap(1000, AiDotLook::TwoEyes, 7));
  EXPECT_EQ(model.Idle(), AiDotIdle::Poke);
  EXPECT_FALSE(model.Tap(1100, AiDotLook::TwoEyes, 9)) << "a tap while it moves is ignored";
  const int64_t poke_end = 1000 + AiDotModel::kPokeMs + AiDotModel::kIdleTailMs;
  EXPECT_EQ(model.MsUntilNext(1000), poke_end - 1000);
  EXPECT_FALSE(model.Advance(poke_end - 1));
  ASSERT_TRUE(model.Advance(poke_end));
  const AiDotIdle action = model.Idle();
  EXPECT_NE(action, AiDotIdle::None);
  const auto rotation = AiDotModel::IdleActions(AiDotLook::TwoEyes);
  EXPECT_NE(std::find(rotation.begin(), rotation.end(), action), rotation.end()) << "one of the dot's face actions";
  const int64_t end = poke_end + AiDotModel::IdleLengthMs(action, AiDotLook::TwoEyes) + AiDotModel::kIdleTailMs;
  ASSERT_TRUE(model.Advance(end));
  EXPECT_EQ(model.Idle(), AiDotIdle::None);
  EXPECT_FALSE(model.MsUntilNext(end).has_value());
  EXPECT_TRUE(model.Tap(end + 10, AiDotLook::TwoEyes, 1)) << "tappable again";
}

TEST(HomeMascotModelTest, DifferentRollsPlayDifferentActions) {
  std::set<AiDotIdle> seen;
  for (uint32_t roll = 0; roll < 24; ++roll) {
    HomeMascotModel model;
    model.Tap(0, AiDotLook::Plain, roll);
    model.Advance(AiDotModel::kPokeCalmMs + AiDotModel::kIdleTailMs);
    seen.insert(model.Idle());
  }
  EXPECT_GT(seen.size(), 3u);
  EXPECT_EQ(seen.count(AiDotIdle::Blink), 0u) << "the plain look has no eyes to blink";
}

TEST(AiDotActionLengthTest, EveryActionIsAboutThirtyPercentLongerThanBefore) {
  // Kenneth 2026-11-09: "动作再长一点点" (the values before were 1200, 500, 2000, … and the RCSS seconds follow: the
  // keyframes test in ai_dot_model_test ties each model length to its animation).
  EXPECT_EQ(AiDotModel::kSwayMs, 1560);
  EXPECT_EQ(AiDotModel::kBlinkMs, 650);
  EXPECT_EQ(AiDotModel::kLookMs, 2600);
  EXPECT_EQ(AiDotModel::kTalkMs, 2184);
  EXPECT_EQ(AiDotModel::kShyMs, 2080);
  EXPECT_EQ(AiDotModel::kWaveMs, 2080);
  EXPECT_EQ(AiDotModel::kWaveCalmMs, 2860);
  EXPECT_EQ(AiDotModel::kListenMs, 3120);
  EXPECT_EQ(AiDotModel::kThinkMs, 3120);
  EXPECT_EQ(AiDotModel::kFiveMs, 1950);
  EXPECT_EQ(AiDotModel::kHappyMs, 2340);
  EXPECT_EQ(AiDotModel::kPrivateMs, 2600);
  EXPECT_EQ(AiDotModel::kSleepMs, 5200);
  EXPECT_EQ(AiDotModel::kPokeMs, 780);
  EXPECT_EQ(AiDotModel::kPokeCalmMs, 585);
  EXPECT_EQ(AiDotModel::kPopMs, 910);
  EXPECT_EQ(AiDotModel::kFloatMs, 3250);
  EXPECT_EQ(AiDotModel::kIdleMinMs, 3000) << "the pause between actions is unchanged";
  EXPECT_EQ(AiDotModel::kIdleMaxMs, 6000);
  EXPECT_EQ(AiDotModel::kSleepAfterMs, 120000);
}

TEST(MascotFigureTest, EveryLookHasTheLayersTheShellDotDraws) {
  // The mascot's figure is built from MascotLayers(); the shell's dot has the same layers in window_shell.rml. They must
  // match look by look, in order, or the two would drift apart.
  const std::string shell = ReadAsset("samples/window_shell.rml");
  ASSERT_FALSE(shell.empty());
  const std::regex layer(R"re(<svg class="ai-dot-layer([^"]*)" data-if="([^"]+)" src="\.\./icons/mascot/([^"]+)")re");
  struct Entry {
    std::string classes;
    std::string cond;
    std::string src;
  };
  std::vector<Entry> entries;
  for (auto it = std::sregex_iterator(shell.begin(), shell.end(), layer); it != std::sregex_iterator(); ++it) {
    std::string classes = (*it)[1];
    if (!classes.empty() && classes[0] == ' ') {
      classes.erase(0, 1);
    }
    entries.push_back({classes, (*it)[2], (*it)[3]});
  }
  ASSERT_FALSE(entries.empty());
  const auto in_look = [](const std::string& cond, const char look) {
    if (cond.find("!=") != std::string::npos) {
      return cond.find(std::string("'") + look + "'") == std::string::npos;
    }
    return cond.find(std::string("== '") + look + "'") != std::string::npos;
  };
  for (const char look : {'a', 'b', 'c', 'd'}) {
    std::vector<std::string> from_shell;
    for (const Entry& entry : entries) {
      if (in_look(entry.cond, look)) {
        from_shell.push_back(entry.classes + "|" + entry.src);
      }
    }
    std::vector<std::string> from_table;
    for (int part = 0; part < 3; ++part) {
      for (const MascotLayer& layer_row : MascotLayers()) {
        if (layer_row.part == part && std::string(layer_row.looks).find(look) != std::string::npos) {
          from_table.push_back(std::string(layer_row.classes) + "|" + layer_row.src);
        }
      }
    }
    EXPECT_EQ(from_table, from_shell) << "look " << look;
    for (const MascotLayer& layer_row : MascotLayers()) {
      EXPECT_TRUE(std::ifstream(std::string(PP_BROWSER_ASSETS_DIR) + "/icons/mascot/" + layer_row.src).good())
          << layer_row.src;
    }
    const std::string rml = MascotFigureRml(look);
    EXPECT_NE(rml.find("ai-dot-figure"), std::string::npos);
    EXPECT_NE(rml.find("ai-dot-back"), std::string::npos);
    EXPECT_EQ(std::count(from_table.begin(), from_table.end(), std::string()), 0);
  }
}

TEST(HomeMarkupTest, TheLogoStartsAsTheStaticIconAndTheMascotIsTheDotsFigure) {
  const std::string home = ReadAsset("views/home.rml");
  EXPECT_NE(home.find("<div class=\"home-logo\" id=\"home-logo\"><img src=\"../branding/app-icon.png\""),
            std::string::npos)
      << "off (and before the mascot mounts) it is the static icon";
  EXPECT_NE(ShellHomeMascot::StaticRml(), nullptr);
  EXPECT_NE(std::string(ShellHomeMascot::StaticRml()).find("app-icon.png"), std::string::npos);
  const std::string two_eyes = ShellHomeMascot::MascotRml('b');
  EXPECT_NE(two_eyes.find("id=\"home-mascot\""), std::string::npos);
  EXPECT_NE(two_eyes.find("b-eyes-front.svg"), std::string::npos);
  EXPECT_EQ(two_eyes.find("ai-dot--calm"), std::string::npos);
  const std::string saturday = ShellHomeMascot::MascotRml('f');
  EXPECT_NE(saturday.find("a-eyes-front.svg"), std::string::npos) << "f borrows one eye until its parts exist";
  const std::string plain = ShellHomeMascot::MascotRml('d');
  EXPECT_NE(plain.find("ai-dot--calm"), std::string::npos);
  EXPECT_NE(plain.find("ai-dot--plain"), std::string::npos);
}

// The DOM side, over the real stylesheets.
class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i& dimensions, const ui::String&) override {
    dimensions = {64, 64};
    return 2;
  }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

class FakeClock : public ui::SystemInterface {
public:
  double GetElapsedTime() override { return now; }
  double now = 100.0;
};

class ShellHomeMascotTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&render);
    ui::SetSystemInterface(&clock);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("home-mascot-test", {400, 800});
    ASSERT_NE(context, nullptr);
    const std::string rml =
        "<rml><head><link type=\"text/rcss\" href=\"components.rcss\"/></head><body>"
        "<div id=\"outer\"><div class=\"home-logo\" id=\"home-logo\">" +
        std::string(ShellHomeMascot::StaticRml()) + "</div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml, std::string(PP_BROWSER_ASSETS_DIR) + "/themes/home_mascot_test.rml");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
  }
  void TearDown() override {
    if (context) {
      ui::RemoveContext("home-mascot-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetSystemInterface(nullptr);
    ui::SetRenderInterface(nullptr);
  }

  NullRender render;
  FakeClock clock;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
};

TEST_F(ShellHomeMascotTest, OnItReplacesTheIconWithTodaysFigureAndOffBringsTheIconBack) {
  ShellHomeMascot mascot;
  mascot.SetDateForTest(2026, 11, 10); // a Tuesday: b
  mascot.Update(context);
  ui::Element* logo = document->GetElementById("home-logo");
  ASSERT_NE(logo, nullptr);
  ui::Element* figure = document->GetElementById("home-mascot");
  ASSERT_NE(figure, nullptr) << "the mascot is mounted";
  EXPECT_TRUE(logo->IsClassSet("home-logo--mascot"));
  EXPECT_TRUE(figure->IsClassSet("ai-dot"));
  EXPECT_NE(logo->GetInnerRML().find("b-eyes-front.svg"), std::string::npos);
  EXPECT_FALSE(mascot.MsUntilNext().has_value()) << "nothing animates until a tap";

  mascot.SetAnimated(false);
  mascot.Update(context);
  EXPECT_EQ(document->GetElementById("home-mascot"), nullptr);
  EXPECT_FALSE(logo->IsClassSet("home-logo--mascot"));
  EXPECT_NE(logo->GetInnerRML().find("app-icon.png"), std::string::npos) << "off: the static icon";
  EXPECT_FALSE(mascot.MsUntilNext().has_value());
}

TEST_F(ShellHomeMascotTest, TheLookFollowsThePrefAndTheDate) {
  ShellHomeMascot mascot;
  mascot.SetDateForTest(2026, 11, 9); // Monday: a
  mascot.SetLookPref("auto");
  mascot.Update(context);
  ui::Element* logo = document->GetElementById("home-logo");
  EXPECT_NE(logo->GetInnerRML().find("a-eyes-front.svg"), std::string::npos);
  mascot.SetDateForTest(2026, 11, 11); // Wednesday: c
  mascot.Update(context);
  EXPECT_NE(logo->GetInnerRML().find("c-eyes-front.svg"), std::string::npos) << "auto: the day turned over";
  mascot.SetLookPref("d");
  mascot.Update(context);
  EXPECT_NE(logo->GetInnerRML().find("d-line-1.svg"), std::string::npos) << "a picked look wins";
  mascot.SetDateForTest(2026, 11, 12);
  mascot.SetLookPref("b");
  mascot.Update(context);
  EXPECT_NE(logo->GetInnerRML().find("b-eyes-front.svg"), std::string::npos);
}

TEST_F(ShellHomeMascotTest, ATapPlaysAnActionAndIsTakenByTheLogoAlone) {
  ShellHomeMascot mascot;
  mascot.SetDateForTest(2026, 11, 10);
  mascot.Update(context);
  ui::Element* outer = document->GetElementById("outer");
  struct Spy : ui::EventListener {
    void ProcessEvent(ui::Event&) override { ++clicks; }
    int clicks = 0;
  } spy;
  outer->AddEventListener(ui::EventId::Click, &spy); // the home page, the drawer's gestures: must not see the tap
  ui::Element* logo = document->GetElementById("home-logo");
  logo->DispatchEvent(ui::EventId::Click, {});
  EXPECT_EQ(spy.clicks, 0) << "the tap stops at the logo: no drawer, no ask panel";
  EXPECT_TRUE(document->GetElementById("home-mascot")->IsClassSet("ai-dot--poke")) << "it pokes first";
  EXPECT_TRUE(mascot.MsUntilNext().has_value()) << "one frame is asked for when the action ends";
  outer->RemoveEventListener(ui::EventId::Click, &spy);
}

TEST_F(ShellHomeMascotTest, ATapOnTheStaticIconDoesNothing) {
  ShellHomeMascot mascot;
  mascot.SetAnimated(false);
  mascot.Update(context);
  document->GetElementById("home-logo")->DispatchEvent(ui::EventId::Click, {});
  EXPECT_FALSE(mascot.MsUntilNext().has_value());
}

} // namespace
} // namespace pbr
