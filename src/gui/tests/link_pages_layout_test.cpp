// The two "link a new device" pages on the real stylesheets, in English and Chinese, on a 390dp phone and a 1100dp
// desktop window: the new device's page (the PIN gate's link step, as ShellLayoutRml writes it) and the main device's
// page (settings_security_transfer.rml). Per state (no camera / a camera / a reply pasted or shown) exactly one primary
// button is on screen, nothing overlaps, the code is centred and as large as its display rule says, and every tap
// target is at least 44dp. The `data-if` / `{{…}}` bindings are evaluated here from a small table, so the page's own
// markup is what is measured.
#include "gui/QrDisplay.h"
#include "gui/shell/ShellLayoutRml.h"
#include "foundation/i18n/LocalizationService.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <ui/style/ComputedValues.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

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

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

std::string AssetPath(const std::string& relative) {
  return std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative;
}

// --- binding evaluation ---------------------------------------------------------------------------------------------

using Vars = std::map<std::string, std::string>; // "1" / "0" for flags, text otherwise

class Expr {
public:
  Expr(const std::string& text, const Vars& vars) : text_(text), vars_(vars) {}
  bool Eval() {
    const bool value = Or();
    return value;
  }

private:
  struct Value {
    std::string text;
    bool truthy() const { return !text.empty() && text != "0"; }
  };
  void Skip() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
  }
  bool Eat(const char* token) {
    Skip();
    const std::string t(token);
    if (text_.compare(pos_, t.size(), t) == 0) {
      pos_ += t.size();
      return true;
    }
    return false;
  }
  bool Or() {
    bool value = And();
    while (Eat("||")) {
      const bool rhs = And();
      value = value || rhs;
    }
    return value;
  }
  bool And() {
    bool value = Not();
    while (Eat("&&")) {
      const bool rhs = Not();
      value = value && rhs;
    }
    return value;
  }
  bool Not() {
    Skip();
    if (pos_ < text_.size() && text_[pos_] == '!' && !(pos_ + 1 < text_.size() && text_[pos_ + 1] == '=')) {
      ++pos_;
      return !Not();
    }
    return Cmp();
  }
  bool Cmp() {
    const Value lhs = Atom();
    if (Eat("!=")) {
      return lhs.text != Atom().text;
    }
    if (Eat("==")) {
      return lhs.text == Atom().text;
    }
    return lhs.truthy();
  }
  Value Atom() {
    Skip();
    if (Eat("(")) {
      const bool inner = Or();
      Eat(")");
      return {inner ? "1" : "0"};
    }
    if (pos_ < text_.size() && text_[pos_] == '\'') {
      const size_t end = text_.find('\'', pos_ + 1);
      Value value{text_.substr(pos_ + 1, end - pos_ - 1)};
      pos_ = end + 1;
      return value.text.empty() ? Value{""} : Value{"s:" + value.text};
    }
    const size_t start = pos_;
    while (pos_ < text_.size() &&
           (std::isalnum(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '_' || text_[pos_] == '.')) {
      ++pos_;
    }
    const std::string name = text_.substr(start, pos_ - start);
    const auto it = vars_.find(name);
    EXPECT_NE(it, vars_.end()) << "unbound name in a data-if: " << name;
    return {it == vars_.end() ? "" : it->second};
  }
  std::string text_;
  const Vars& vars_;
  size_t pos_ = 0;
};

// The markup with its bindings applied: false `data-if`s become display:none, `{{x}}` and `{{i18n:key}}` their text,
// the bound code size an inline style, and the event / value attributes (no data model here) go.
std::string Bind(std::string rml, const Vars& vars) {
  static const std::regex if_attr(R"re( data-if="([^"]*)")re");
  std::string out;
  std::smatch match;
  while (std::regex_search(rml, match, if_attr)) {
    out += match.prefix();
    const std::string expr = match[1];
    out += Expr(expr, vars).Eval() ? "" : " style=\"display: none;\"";
    rml = match.suffix();
  }
  out += rml;
  static const std::regex size_attrs(R"re( data-style-width="(\w+)" data-style-height="(\w+)")re");
  const auto at = [&vars](const std::string& name) {
    const auto it = vars.find(name);
    return it == vars.end() ? std::string("0dp") : it->second;
  };
  std::string sized;
  while (std::regex_search(out, match, size_attrs)) {
    sized += match.prefix();
    sized += " style=\"width: " + at(match[1]) + "; height: " + at(match[2]) + ";\"";
    out = match.suffix();
  }
  sized += out;
  static const std::regex drop(R"re( (data-event-[a-z]+|data-value|data-rml|data-model)="[^"]*")re");
  sized = std::regex_replace(sized, drop, "");
  static const std::regex text_var(R"re(\{\{(?!i18n:)(\w+)\}\})re");
  std::string text;
  while (std::regex_search(sized, match, text_var)) {
    text += match.prefix();
    text += at(match[1]) == "0dp" ? "" : at(match[1]);
    sized = match.suffix();
  }
  text += sized;
  return LocalizationService::Instance().LocalizeText(text);
}

// --- the page ---------------------------------------------------------------------------------------------------------

class LinkPagesLayoutTest : public testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/Inter-Regular.ttf"), /*fallback_face=*/true));
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/LatoLatin-Regular.ttf")));
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/NotoSansSC-Medium.otf"), /*fallback_face=*/true)); // the Chinese text
  }
  void TearDown() override {
    if (context) {
      ui::RemoveContext("link-pages-layout-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  void Mount(const std::string& body, const int width, const int height, const bool dark = false) {
    width_ = width;
    height_ = height;
    if (context) {
      ui::RemoveContext("link-pages-layout-test");
      context = nullptr;
    }
    context = ui::CreateContext("link-pages-layout-test", {width, height});
    ASSERT_NE(context, nullptr);
    const std::string rml = "<rml><head><style>" + ReadAll(AssetPath("themes/foundation.rcss")) +
                            ReadAll(AssetPath("themes/components.rcss")) +
                            ReadAll(AssetPath(dark ? "themes/colors-dark.rcss" : "themes/colors-light.rcss")) +
                            "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
                            "<div style=\"position: absolute; top: 0; left: 0; width: " +
                            std::to_string(width) + "dp; height: " + std::to_string(height) + "dp;\">" + body +
                            "</div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
  }

  static bool Shown(ui::Element* element) {
    for (ui::Element* e = element; e != nullptr; e = e->GetParentNode()) {
      if (e->GetComputedValues().display() == ui::Style::Display::None) {
        return false;
      }
    }
    return true;
  }
  std::vector<ui::Element*> Visible(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    std::vector<ui::Element*> shown;
    for (ui::Element* e : found) {
      if (Shown(e)) {
        shown.push_back(e);
      }
    }
    return shown;
  }
  ui::Element* One(const char* cls) {
    const auto shown = Visible(cls);
    EXPECT_EQ(shown.size(), 1U) << cls;
    return shown.empty() ? nullptr : shown.front();
  }
  struct Edges {
    float left, top, right, bottom;
    float MidX() const { return (left + right) / 2; }
  };
  static Edges Box(ui::Element* element) {
    const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
    return {at.x, at.y, at.x + size.x, at.y + size.y};
  }

  // The middle of the room an element's children have (a scrollbar, when it scrolls, takes its share of the width).
  static float ContentMidX(ui::Element* element) {
    const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Content);
    return at.x + element->GetBox().GetSize(ui::BoxArea::Content).x / 2.f;
  }

  // What the person can tap: every visible button.
  std::vector<ui::Element*> Buttons() {
    ui::ElementList found;
    document->GetElementsByTagName(found, "button");
    std::vector<ui::Element*> shown;
    for (ui::Element* e : found) {
      if (Shown(e)) {
        shown.push_back(e);
      }
    }
    return shown;
  }
  int PrimaryCount() {
    int count = 0;
    for (ui::Element* button : Buttons()) {
      count += button->IsClassSet("btn-primary") ? 1 : 0;
    }
    return count;
  }
  void ExpectTargetsAndNoOverlap(const std::vector<ui::Element*>& stack, const std::string& where) {
    for (ui::Element* button : Buttons()) {
      const std::string label = std::string(button->GetInnerRML().c_str());
      if (button->IsClassSet("settings-q-btn")) {
        continue; // the "?" is a 44dp square by its own rule, checked with the other settings rows
      }
      EXPECT_GE(Box(button).bottom - Box(button).top, 44.f) << where << " target too small: " << label;
    }
    float previous_bottom = -1.f;
    for (ui::Element* block : stack) {
      const Edges box = Box(block);
      EXPECT_GE(box.top + 0.5f, previous_bottom) << where << " overlaps the block above: " << block->GetClassNames();
      previous_bottom = box.bottom;
    }
    EXPECT_LE(Box(stack.back()).right, static_cast<float>(width_) + 0.5f) << where;
  }

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  int width_ = 0;
  int height_ = 0;
};

// --- the new device's page ----------------------------------------------------------------------------------------

struct GateCase {
  const char* name;
  bool can_scan;
  bool reply_pasted;
};

constexpr GateCase kGateCases[] = {
    {"no camera", false, false},
    {"camera", true, false},
    {"reply pasted", true, true},
};

TEST_F(LinkPagesLayoutTest, TheNewDevicesPageHasOnePrimaryButtonAndACentredCodeInEveryState) {
  for (const char* lang : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(lang);
    for (const bool compact : {true, false}) {
      const int width = compact ? 390 : 1100;
      const int height = compact ? 844 : 900;
      for (const GateCase& state_case : kGateCases) {
        const std::string where = std::string(lang) + (compact ? " phone " : " desktop ") + state_case.name;
        ShellState state;
        state.layout_mode = compact ? LayoutMode::Compact : LayoutMode::Expanded;
        state.pin_gate.active = true;
        state.pin_gate.link_paste_mode = true;
        const QrDisplay display = LinkQrDisplayFor(compact, static_cast<float>(width), static_cast<float>(height), 1.f);
        const std::string rml = ShellLayoutRml(state, false, "").PinGate();
        Vars vars{{"pin_gate_title", Tr("pin.link_paste_title")},
                  {"pin_gate_message", Tr("pin.link_paste_message")},
                  {"pin_gate_error", ""},
                  {"pin_gate_biometric_hint", ""},
                  {"pin_gate_identity_fork_mode", "0"},
                  {"pin_gate_chooser_mode", "0"},
                  {"pin_gate_link_paste_mode", "1"},
                  {"pin_gate_phrase_flow", "0"},
                  {"pin_gate_link_offer_qr_rml", "x"},
                  {"pin_gate_link_offer_code", "0427"},
                  {"pin_gate_link_valid", Tr("settings.security.link_device.copied_valid", {{"minutes", "15"}})},
                  {"pin_gate_link_qr_side", display.css},
                  {"pin_gate_link_can_scan", state_case.can_scan ? "1" : "0"},
                  {"pin_gate_payload_hidden", state_case.reply_pasted ? "1" : "0"},
                  {"pin_gate_saved_copy_name", ""},
                  {"pin_gate_default_note_line", ""},
                  {"pin_gate_default_note_open", "0"},
                  {"pin_gate_default_note_text", ""},
                  {"pin_gate_waiting_device_unlock", "0"},
                  {"pin_gate_unlock_mode", "0"},
                  {"pin_gate_forgot_open", "0"},
                  {"pin_gate_create_mode", "0"},
                  {"pin_gate_verify_mode", "0"},
                  {"pin_gate_change_mode", "0"},
                  {"pin_gate_ask_confirm", "0"},
                  {"pin_gate_keypad_offered", "0"},
                  {"pin_gate_biometric_key", "0"},
                  {"pin_gate_biometric_kind", ""},
                  {"pin_gate_link_payload", ""}};
        // The card is laid out as the shell lays a dialog: a layer over the whole window.
        Mount("<div class=\"shell-layer shell-layer-dialog\" style=\"position: absolute; top: 0; left: 0; width: " +
                  std::to_string(width) + "dp; height: " + std::to_string(height) + "dp;\">" +
                  Bind(rml.substr(rml.find("<div class=\"shell-scrim\">")), vars).substr(0),
              width, height);
        ASSERT_NE(document, nullptr) << where;
        ui::Element* card = One("shell-pin-gate");
        ASSERT_NE(card, nullptr) << where;
        EXPECT_EQ(PrimaryCount(), 1) << where << ": one primary button";
        // A phone's card is the whole screen; a desktop's is a card in the middle (its translate(-50%) is a transform,
        // which the offsets measured here leave out).
        if (compact) {
          EXPECT_NEAR(Box(card).MidX(), width / 2.f, 1.f) << where;
        }
        const float centre = ContentMidX(card);
        EXPECT_LE(Box(card).right - Box(card).left, compact ? width + 0.5f : 560.5f) << where;
        if (state_case.reply_pasted) {
          EXPECT_TRUE(Visible("shell-pin-gate-scan").empty()) << where;
        } else {
          EXPECT_EQ(Visible("shell-pin-gate-scan").size(), state_case.can_scan ? 1U : 0U) << where;
          EXPECT_EQ(Visible("shell-pin-gate-paste").size(), 1U) << where;
        }
        ui::Element* qr = One("shell-pin-gate-qr");
        ASSERT_NE(qr, nullptr) << where;
        EXPECT_FLOAT_EQ(Box(qr).right - Box(qr).left, static_cast<float>(display.side_dp)) << where;
        EXPECT_FLOAT_EQ(Box(qr).bottom - Box(qr).top, static_cast<float>(display.side_dp)) << where;
        EXPECT_NEAR(Box(qr).MidX(), centre, 1.f) << where << ": the code is centred";
        EXPECT_LE(Box(qr).right - Box(qr).left, Box(card).right - Box(card).left) << where;
        // The 4 digits sit directly under the code, labelled, centred.
        ui::Element* digits = One("shell-pin-gate-code");
        ASSERT_NE(digits, nullptr) << where;
        EXPECT_NEAR(Box(digits).MidX(), centre, 1.f) << where;
        EXPECT_LE(card->GetScrollHeight(), card->GetClientHeight() + 1.f) << where << ": nothing needs scrolling";
        EXPECT_LT(Box(digits).top - Box(qr).bottom, 60.f) << where << ": the digits are right under the code";
        std::vector<ui::Element*> stack{
            One("shell-dialog-title"),   One("shell-dialog-message"), qr, One("shell-pin-gate-code-label"), digits,
            One("shell-pin-gate-valid"), One("shell-pin-gate-copy")};
        if (!state_case.reply_pasted) {
          stack.push_back(One("shell-pin-gate-step"));
        }
        ExpectTargetsAndNoOverlap(stack, where);
      }
    }
  }
}

TEST(LinkPagesDisplayTest, TheCodeIsDrawnAsLargeAsThePageAllows) {
  // Before: 260dp on the new device's page and 280dp on the main device's, on every form factor (1.8 to 2.0 dp per
  // module of the 141 a version-29 code spans with its quiet zone).
  constexpr float kModules = 141.f;
  // A 1100 x 900 desktop window: 3 dp per module (423dp).
  const QrDisplay desktop = LinkQrDisplayFor(false, 1100.f, 900.f, 1.f);
  EXPECT_EQ(desktop.side_dp, kLinkQrDesktopMaxDp);
  EXPECT_GE(desktop.side_dp / kModules, 3.f);
  // A short window keeps what room it has, never under the old 280dp.
  EXPECT_EQ(LinkQrDisplayFor(false, 1100.f, 800.f, 1.f).side_dp, 330);
  EXPECT_EQ(LinkQrDisplayFor(false, 1100.f, 500.f, 1.f).side_dp, kLinkQrDesktopMinDp);
  EXPECT_EQ(LinkQrDisplayFor(false, 360.f, 900.f, 1.f).side_dp, kLinkQrDesktopMinDp);
  // The settings page's column is what is left of the window beside the rail and the list.
  EXPECT_EQ(LinkQrDisplayFor(false, 1100.f, 900.f, 1.f, kLinkQrDesktopSettingsReservedWidthDp).side_dp,
            kLinkQrDesktopMaxDp);
  EXPECT_EQ(LinkQrDisplayFor(false, 900.f, 900.f, 1.f, kLinkQrDesktopSettingsReservedWidthDp).side_dp, 400);
  // Phones: the page's width less 20dp a side, at most 340dp (2.4 dp per module, about 7 physical pixels at 3x).
  EXPECT_EQ(LinkQrDisplayFor(true, 390.f, 844.f, 1.f).side_dp, 340);
  EXPECT_EQ(LinkQrDisplayFor(true, 360.f, 640.f, 1.f).side_dp, 320);
  EXPECT_EQ(LinkQrDisplayFor(true, 320.f, 568.f, 1.f).side_dp, 280);
  EXPECT_EQ(LinkQrDisplayFor(true, 390.f, 844.f, 3.f).side_px, 340 * 3);
  EXPECT_GT(LinkQrDisplayFor(true, 390.f, 844.f, 1.f).side_dp, 280) << "bigger than either old size";
}

// --- the main device's page ---------------------------------------------------------------------------------------

struct MainCase {
  const char* name;
  bool can_scan;
  bool reply_shown;
};

constexpr MainCase kMainCases[] = {
    {"no camera", false, false},
    {"camera", true, false},
    {"reply shown", true, true},
};

TEST_F(LinkPagesLayoutTest, TheMainDevicesPageHasOnePrimaryButtonAndACentredReplyCodeInEveryState) {
  for (const char* lang : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(lang);
    for (const bool compact : {true, false}) {
      const int width = compact ? 390 : 760; // the desktop's detail pane beside the section list
      const int height = compact ? 844 : 900;
      for (const MainCase& state_case : kMainCases) {
        const std::string where = std::string(lang) + (compact ? " phone " : " desktop ") + state_case.name;
        const QrDisplay display =
            LinkQrDisplayFor(compact, static_cast<float>(compact ? 390 : 1100), static_cast<float>(height), 1.f);
        const std::string minutes = "15";
        Vars vars{{"security_can_export_link", "1"},
                  {"own_devices_not_main", "0"},
                  {"link_reply_shown", state_case.reply_shown ? "1" : "0"},
                  {"link_can_scan", state_case.can_scan ? "1" : "0"},
                  {"move_lead_label", Tr("settings.security.link_device.lead")},
                  {"move_valid_label", Tr("settings.security.link_device.copied_valid", {{"minutes", minutes}})},
                  {"link_qr_side", display.css},
                  {"link_reply_qr_rml", "x"}};
        const std::string page = Bind(ReadAll(AssetPath("views/settings_security_transfer.rml")), vars);
        const std::string wrapper = compact
                                        ? "<div class=\"shell-layer shell-layer-compact\">"
                                          "<div class=\"settings-subpage-view\" style=\"position: absolute; top: 0; "
                                          "left: 0; width: 390dp; height: 844dp; overflow-y: auto;\">"
                                        : "<div class=\"settings-detail-view\" style=\"position: absolute; top: 0; "
                                          "left: 0; width: 760dp; height: 900dp; overflow-y: auto;\">";
        Mount(wrapper + page + (compact ? "</div></div>" : "</div>"), width, height);
        ASSERT_NE(document, nullptr) << where;
        EXPECT_EQ(PrimaryCount(), 1) << where << ": one primary button";
        if (state_case.reply_shown) {
          ui::Element* qr = One("settings-link-qr");
          ASSERT_NE(qr, nullptr) << where;
          EXPECT_FLOAT_EQ(Box(qr).right - Box(qr).left, static_cast<float>(display.side_dp)) << where;
          ui::Element* box = One("settings-link-reply");
          ASSERT_NE(box, nullptr) << where;
          EXPECT_NEAR(Box(qr).MidX(), Box(box).MidX(), 1.f) << where << ": the code is centred";
          EXPECT_EQ(Visible("settings-link-btn").size(), 2U) << where << ": copy and done";
          EXPECT_TRUE(Visible("settings-transfer-btn").empty()) << where;
          EXPECT_TRUE(Visible("settings-transfer-lead").empty()) << where << ": no lead over the reply";
          ExpectTargetsAndNoOverlap({One("settings-copied-title"), qr, One("settings-copied-sub")}, where);
        } else {
          const auto buttons = Visible("settings-transfer-btn");
          ASSERT_EQ(buttons.size(), state_case.can_scan ? 2U : 1U) << where << ": scan and paste, or paste alone";
          EXPECT_EQ(Visible("settings-transfer-lead").size(), 1U) << where;
          EXPECT_TRUE(Visible("settings-link-qr").empty()) << where;
          ExpectTargetsAndNoOverlap({One("settings-transfer-lead")}, where);
          if (buttons.size() == 2) {
            EXPECT_GE(Box(buttons[1]).top + 0.5f, Box(buttons[0]).bottom) << where << ": the buttons do not overlap";
          }
          EXPECT_TRUE(buttons[0]->IsClassSet("btn-primary")) << where << ": the first button is the main one";
        }
      }
    }
  }
}

} // namespace
} // namespace pbr
