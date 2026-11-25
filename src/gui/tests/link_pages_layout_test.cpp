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
  static const std::regex width_only(R"re( data-style-width="(\w+)")re");
  out = sized;
  sized.clear();
  while (std::regex_search(out, match, width_only)) {
    sized += match.prefix();
    sized += " style=\"width: " + at(match[1]) + ";\"";
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


// The new device's page as the shell binds it: the code's side, whether a camera is listed, whether a reply was pasted,
// and the error line (empty: none).
Vars GateVars(const std::string& side, const bool can_scan, const bool reply_pasted, const std::string& error) {
  return {{"pin_gate_title", Tr("pin.link_paste_title")},
          {"pin_gate_message", Tr("pin.link_paste_message")},
          {"pin_gate_error", error},
          {"pin_gate_biometric_hint", ""},
          {"pin_gate_identity_fork_mode", "0"},
          {"pin_gate_chooser_mode", "0"},
          {"pin_gate_link_paste_mode", "1"},
          {"pin_gate_phrase_flow", "0"},
          {"pin_gate_link_offer_qr_rml", "x"},
          {"pin_gate_link_offer_code", "0427"},
          {"pin_gate_link_valid", Tr("settings.security.link_device.copied_valid", {{"minutes", "15"}})},
          {"pin_gate_link_qr_side", side},
          {"pin_gate_link_can_scan", can_scan ? "1" : "0"},
          {"pin_gate_payload_hidden", reply_pasted ? "1" : "0"},
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
  // The room inside an element's padding: its content box.
  static Edges ContentBox(ui::Element* element) {
    const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Content);
    const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Content);
    return {at.x, at.y, at.x + size.x, at.y + size.y};
  }
  // Every visible button, and the given extra blocks, lies inside the card's content box.
  void ExpectInside(ui::Element* card, const std::vector<ui::Element*>& extra, const std::string& where) {
    const Edges room = ContentBox(card);
    std::vector<ui::Element*> all = Buttons();
    all.insert(all.end(), extra.begin(), extra.end());
    for (ui::Element* e : all) {
      const Edges box = Box(e);
      const std::string who = where + ": " + e->GetClassNames() + " [" + std::string(e->GetInnerRML().c_str()) + "]";
      EXPECT_GE(box.left + 0.5f, room.left) << who << " sticks out on the left";
      EXPECT_LE(box.right, room.right + 0.5f) << who << " sticks out on the right";
    }
  }
  // Two buttons in one row: same top and height, same width, side by side with a gap.
  void ExpectOneRow(ui::Element* first, ui::Element* second, const std::string& where) {
    const Edges a = Box(first);
    const Edges b = Box(second);
    EXPECT_NEAR(a.top, b.top, 0.5f) << where << ": one row";
    EXPECT_NEAR(a.bottom, b.bottom, 0.5f) << where << ": same height";
    EXPECT_NEAR(a.right - a.left, b.right - b.left, 1.f) << where << ": equal widths";
    EXPECT_GE(b.left - a.right, 4.f) << where << ": a gap between them";
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
    // (A desktop card's translate(-50%) is a transform the offsets leave out, so the width is what is compared.)
    EXPECT_LE(Box(stack.back()).right - Box(stack.back()).left, static_cast<float>(width_) + 0.5f) << where;
  }

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  int width_ = 0;
  int height_ = 0;
};

// `computer`: a desktop computer (Platform::IsDesktop; ShellState::desktop_computer), whatever the window's layout. A
// phone and an iPad (even expanded, 1024x768) are not; a narrow desktop window (500 wide, the compact layout) is.
struct WindowSize {
  bool compact;
  int width;
  int height;
  bool computer;
};

constexpr WindowSize kWindowSizes[] = {{true, 390, 844, false}, {true, 430, 932, false},   {false, 1100, 900, true},
                                       {false, 900, 640, true}, {false, 1024, 768, false}, {true, 500, 800, true}};

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
    for (const WindowSize& size : kWindowSizes) {
      const bool compact = size.compact;
      const bool computer = size.computer;
      if (computer) {
        continue; // a computer's page is its own layout: TheNewDevicesPageOnAComputerFitsTheWindowInEveryState
      }
      const int width = size.width;
      const int height = size.height;
      for (const GateCase& state_case : kGateCases) {
        const std::string where = std::string(lang) +
                                  (computer  ? " computer "
                                   : compact ? " phone "
                                             : " tablet ") +
                                  std::to_string(width) + "x" + std::to_string(height) + " " + state_case.name;
        ShellState state;
        state.desktop_computer = computer;
        state.layout_mode = compact ? LayoutMode::Compact : LayoutMode::Expanded;
        state.pin_gate.active = true;
        state.pin_gate.link_paste_mode = true;
        const QrDisplay display = LinkQrDisplayFor(compact, static_cast<float>(width), static_cast<float>(height), 1.f);
        const std::string rml = ShellLayoutRml(state, false, "").PinGate();
        const Vars vars = GateVars(display.css, state_case.can_scan, state_case.reply_pasted, "");
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
        // A short desktop window (640dp) keeps the code's 280dp minimum and scrolls the card; its scrollbar takes 8dp.
        const bool scrolls = (!compact && height < 700) || (compact && computer);
        const float centre = ContentMidX(card);
        const float centre_slack = scrolls ? 9.f : 1.f;
        EXPECT_LE(Box(card).right - Box(card).left, compact ? width + 0.5f : 560.5f) << where;
        // Step markers: step 1 above the code always, step 2 over the buttons until a reply was pasted.
        EXPECT_EQ(Visible("settings-step-num").size(), state_case.reply_pasted ? 1U : 2U) << where << ": step markers";
        if (state_case.reply_pasted) {
          EXPECT_TRUE(Visible("shell-pin-gate-scan").empty()) << where;
          EXPECT_TRUE(Visible("shell-pin-gate-computer-note").empty()) << where;
        } else {
          EXPECT_EQ(Visible("shell-pin-gate-scan").size(), state_case.can_scan ? 1U : 0U) << where;
          EXPECT_EQ(Visible("shell-pin-gate-paste").size(), 1U) << where;
          // A computer's return leg: Paste first and the main one, Scan second, one caption; a phone or a tablet: Scan
          // first and no caption.
          EXPECT_EQ(Visible("shell-pin-gate-computer-note").size(), computer ? 1U : 0U) << where;
          if (state_case.can_scan) {
            ui::Element* scan = One("shell-pin-gate-scan");
            ui::Element* paste = One("shell-pin-gate-paste");
            ExpectOneRow(computer ? paste : scan, computer ? scan : paste, where);
            EXPECT_TRUE((computer ? paste : scan)->IsClassSet("btn-primary")) << where << ": the first button is main";
            EXPECT_FALSE((computer ? scan : paste)->IsClassSet("btn-primary")) << where;
          }
        }
        ui::Element* qr = One("shell-pin-gate-qr");
        ASSERT_NE(qr, nullptr) << where;
        EXPECT_FLOAT_EQ(Box(qr).right - Box(qr).left, static_cast<float>(display.side_dp)) << where;
        EXPECT_FLOAT_EQ(Box(qr).bottom - Box(qr).top, static_cast<float>(display.side_dp)) << where;
        EXPECT_NEAR(Box(qr).MidX(), centre, centre_slack) << where << ": the code is centred";
        EXPECT_LE(Box(qr).right - Box(qr).left, Box(card).right - Box(card).left) << where;
        // The 4 digits sit directly under the code, labelled, centred.
        ui::Element* digits = One("shell-pin-gate-code");
        ASSERT_NE(digits, nullptr) << where;
        EXPECT_NEAR(Box(digits).MidX(), centre, centre_slack) << where;
        if (!scrolls) {
          EXPECT_LE(card->GetScrollHeight(), card->GetClientHeight() + 1.f) << where << ": nothing needs scrolling";
        }
        EXPECT_LT(Box(digits).top - Box(qr).bottom, 60.f) << where << ": the digits are right under the code";
        EXPECT_TRUE(Visible("shell-dialog-message").empty()) << where << ": the message is step 1 on this page";
        ui::Element* step_one = Visible("link-step").front();
        std::vector<ui::Element*> stack{One("shell-dialog-title"),        step_one, qr,
                                        One("shell-pin-gate-code-label"), digits,   One("shell-pin-gate-valid"),
                                        One("shell-pin-gate-copy")};
        std::vector<ui::Element*> inside{qr, digits, step_one};
        if (!state_case.reply_pasted) {
          stack.push_back(One("shell-pin-gate-step"));
          inside.push_back(One("shell-pin-gate-step"));
          if (computer) {
            ui::Element* note = One("shell-pin-gate-computer-note");
            inside.push_back(note);
            // Not clipped: the whole caption is inside the card (a card that scrolls shows the rest on scrolling).
            EXPECT_GE(Box(note).bottom - Box(note).top, 18.f) << where << ": the caption has height";
            if (!scrolls) {
              EXPECT_LE(Box(note).bottom, ContentBox(card).bottom + 0.5f) << where << ": the caption is not clipped";
            }
          }
        }
        ExpectTargetsAndNoOverlap(stack, where);
        ExpectInside(card, inside, where);
        if (!state_case.reply_pasted) {
          for (ui::Element* button : Buttons()) {
            EXPECT_EQ(button->GetInnerRML().find('\n'), std::string::npos) << where;
          }
        }
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

// --- the new device's page on a desktop computer ------------------------------------------------------------------------
//
// The first version of this page was checked only against its card, and the card scrolled: on a 1024x640 window the
// caption and Back sat below the window and no assertion saw it. Here every element is checked against the WINDOW.

struct DesktopWindow {
  int width;
  int height;
};
constexpr DesktopWindow kDesktopWindows[] = {{1280, 800}, {1100, 700}, {1024, 640}, {900, 600}, {800, 560}, {500, 800}};

const char* const kGateErrorKeys[] = {"pin.link_offer_failed",   "pin.link_err_expired",   "pin.link_err_not_for_this_device",
                                      "pin.link_err_unreadable", "pin.link_err_newer",     "pin.link_paste_empty"};

// Every shown element under `root` (text nodes have no box of their own and are left out).
void CollectShown(ui::Element* root, std::vector<ui::Element*>& out) {
  for (int i = 0; i < root->GetNumChildren(); ++i) {
    ui::Element* child = root->GetChild(i);
    if (child->GetTagName().empty() || child->GetTagName()[0] == '#') {
      continue;
    }
    if (child->GetComputedValues().display() == ui::Style::Display::None) {
      continue;
    }
    out.push_back(child);
    CollectShown(child, out);
  }
}

TEST_F(LinkPagesLayoutTest, TheNewDevicesPageOnAComputerFitsTheWindowInEveryState) {
  struct Case {
    const char* name;
    bool can_scan;
    bool reply_pasted;
    const char* error_key; // nullptr: no error line
  };
  std::vector<Case> cases{{"no camera", false, false, nullptr},
                          {"camera", true, false, nullptr},
                          {"reply pasted", true, true, nullptr}};
  for (const char* key : kGateErrorKeys) {
    cases.push_back({key, true, false, key});
  }
  for (const char* lang : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(lang);
    for (const DesktopWindow& window : kDesktopWindows) {
      const LinkComputerLayout layout =
          LinkComputerLayoutFor(static_cast<float>(window.width), static_cast<float>(window.height));
      for (const Case& state_case : cases) {
        const std::string where = std::string(lang) + " computer " + std::to_string(window.width) + "x" +
                                  std::to_string(window.height) + " " + state_case.name +
                                  (layout.two_columns ? " (two columns)" : " (one column)");
        ShellState state;
        state.desktop_computer = true;
        state.link_two_columns = layout.two_columns;
        state.layout_mode = window.width < 600 ? LayoutMode::Compact : LayoutMode::Expanded;
        state.pin_gate.active = true;
        state.pin_gate.link_paste_mode = true;
        const std::string rml = ShellLayoutRml(state, false, "").PinGate();
        const Vars vars = GateVars(std::to_string(layout.side_dp) + "dp", state_case.can_scan, state_case.reply_pasted,
                                   state_case.error_key ? Tr(state_case.error_key) : "");
        Mount(Bind(rml, vars), window.width, window.height);
        ASSERT_NE(document, nullptr) << where;
        ui::Element* card = One("shell-pin-gate");
        ASSERT_NE(card, nullptr) << where;
        ui::Element* body = One("shell-pin-gate-body");
        ASSERT_NE(body, nullptr) << where;
        EXPECT_EQ(PrimaryCount(), 1) << where << ": one primary button";

        // EVERY shown element lies inside the window: this is the assertion the card-only checks lacked.
        std::vector<ui::Element*> shown;
        CollectShown(card, shown);
        const float w = static_cast<float>(window.width);
        const float h = static_cast<float>(window.height);
        for (ui::Element* e : shown) {
          const Edges box = Box(e);
          const std::string who = where + ": " + std::string(e->GetTagName().c_str()) + "." +
                                  std::string(e->GetClassNames().c_str());
          EXPECT_GE(box.left, -0.5f) << who << " is left of the window";
          EXPECT_GE(box.top, -0.5f) << who << " is above the window";
          EXPECT_LE(box.right, w + 0.5f) << who << " is right of the window";
          EXPECT_LE(box.bottom, h + 0.5f) << who << " is below the window";
        }
        EXPECT_LE(Box(card).bottom, h + 0.5f) << where;
        EXPECT_LE(Box(card).right, w + 0.5f) << where;
        // No scrolling anywhere on a window the layout calls normal (1024x640 and larger) -- and on the smaller ones
        // here too, which fit.
        EXPECT_LE(body->GetScrollHeight(), body->GetClientHeight() + 1.f) << where << ": the body needs scrolling";
        EXPECT_LE(card->GetScrollHeight(), card->GetClientHeight() + 1.f) << where << ": the card needs scrolling";

        // The footer: Back always, Continue only once a reply was pasted; both below everything else.
        ui::Element* actions = One("shell-dialog-actions");
        ASSERT_NE(actions, nullptr) << where;
        EXPECT_GE(Box(actions).top + 0.5f, Box(body).bottom) << where << ": the footer is under the body";
        int back = 0;
        for (ui::Element* button : Buttons()) {
          if (button->IsClassSet("btn-secondary") && Box(button).top >= Box(actions).top - 0.5f) {
            ++back;
          }
        }
        EXPECT_EQ(back, 1) << where << ": Back is in the footer";
        // Targets of at least 44dp, and the code is the size the layout computed.
        for (ui::Element* button : Buttons()) {
          EXPECT_GE(Box(button).bottom - Box(button).top, 43.9f) << where << " target too small";
        }
        ui::Element* qr = One("shell-pin-gate-qr");
        ASSERT_NE(qr, nullptr) << where;
        EXPECT_FLOAT_EQ(Box(qr).right - Box(qr).left, static_cast<float>(layout.side_dp)) << where;
        EXPECT_GE(layout.side_dp, kLinkQrDesktopMinDp) << where;
        EXPECT_LE(layout.side_dp, kLinkQrDesktopMaxDp) << where << ": never larger than the cap the page always had";
        ui::Element* digits = One("shell-pin-gate-code");
        ASSERT_NE(digits, nullptr) << where;
        EXPECT_EQ(Visible("shell-pin-gate-computer-note").size(), state_case.reply_pasted ? 0U : 1U) << where;
        if (layout.two_columns) {
          // Code on the left, everything of the steps on the right of it, the footer under both.
          EXPECT_EQ(Visible("shell-pin-gate-cols").size(), 1U) << where;
          for (ui::Element* e : Visible("shell-pin-gate-col-steps")) {
            EXPECT_GE(Box(e).left + 0.5f, Box(qr).right) << where << ": the steps are right of the code";
          }
          EXPECT_GE(Box(digits).left + 0.5f, Box(qr).right) << where << ": the digits are right of the code";
          EXPECT_LE(Box(One("shell-pin-gate-copy")).bottom, Box(body).bottom + 0.5f) << where;
          EXPECT_NEAR(Box(One("shell-pin-gate-copy")).MidX(), Box(qr).MidX(), 1.f) << where << ": Copy is under the code";
        } else {
          EXPECT_TRUE(Visible("shell-pin-gate-cols").empty()) << where;
          EXPECT_NEAR(Box(qr).MidX(), ContentMidX(body), 1.f) << where << ": the code is centred";
          EXPECT_GE(Box(digits).top, Box(qr).bottom) << where << ": the digits are under the code";
        }
      }
    }
  }
}

// A window too small for the 280dp code: the body scrolls, the footer does not move.
TEST_F(LinkPagesLayoutTest, TheNewDevicesPageOnAVeryShortWindowScrollsItsBodyAndKeepsBack) {
  LocalizationService::Instance().SetPreferredLanguage("en");
  for (const DesktopWindow& window : {DesktopWindow{900, 380}, DesktopWindow{600, 420}}) {
    const LinkComputerLayout layout =
        LinkComputerLayoutFor(static_cast<float>(window.width), static_cast<float>(window.height));
    ShellState state;
    state.desktop_computer = true;
    state.link_two_columns = layout.two_columns;
    state.pin_gate.active = true;
    state.pin_gate.link_paste_mode = true;
    const std::string where = std::to_string(window.width) + "x" + std::to_string(window.height);
    Mount(Bind(ShellLayoutRml(state, false, "").PinGate(),
               GateVars(std::to_string(layout.side_dp) + "dp", true, false, "")),
          window.width, window.height);
    ASSERT_NE(document, nullptr) << where;
    ui::Element* card = One("shell-pin-gate");
    ui::Element* body = One("shell-pin-gate-body");
    ASSERT_NE(card, nullptr) << where;
    ASSERT_NE(body, nullptr) << where;
    EXPECT_LE(Box(card).bottom, window.height + 0.5f) << where << ": the card is inside the window";
    EXPECT_GT(body->GetScrollHeight(), body->GetClientHeight()) << where << ": the body scrolls";
    ui::Element* actions = One("shell-dialog-actions");
    ASSERT_NE(actions, nullptr) << where;
    EXPECT_LE(Box(actions).bottom, window.height + 0.5f) << where << ": Back stays on screen";
    EXPECT_GE(Box(actions).top, Box(body).bottom - 0.5f) << where;
    EXPECT_GE(Box(One("shell-dialog-title")).top, -0.5f) << where;
  }
}

TEST(LinkPagesDisplayTest, TheComputerPageFollowsTheWindow) {
  // Wide and tall enough: the code beside the steps, at most the cap the page always had.
  const LinkComputerLayout big = LinkComputerLayoutFor(1280.f, 800.f);
  EXPECT_TRUE(big.two_columns);
  EXPECT_EQ(big.side_dp, kLinkQrDesktopMaxDp);
  // Never taller than the window and never under the minimum.
  for (const DesktopWindow& window : kDesktopWindows) {
    const LinkComputerLayout layout =
        LinkComputerLayoutFor(static_cast<float>(window.width), static_cast<float>(window.height));
    EXPECT_GE(layout.side_dp, kLinkQrDesktopMinDp);
    EXPECT_LE(layout.side_dp, kLinkQrDesktopMaxDp);
  }
  // A narrow window stacks the page; a short one shrinks the code to 280dp.
  EXPECT_FALSE(LinkComputerLayoutFor(500.f, 800.f).two_columns);
  EXPECT_TRUE(LinkComputerLayoutFor(800.f, 560.f).two_columns);
  EXPECT_EQ(LinkComputerLayoutFor(1100.f, 300.f).side_dp, kLinkQrDesktopMinDp);
}

// --- the main device's page ---------------------------------------------------------------------------------------

struct MainCase {
  const char* name;
  bool can_scan;
  bool reply_shown;
  bool can_share;
};

constexpr MainCase kMainCases[] = {
    {"no camera", false, false, false},
    {"camera", true, false, false},
    {"reply shown, no share sheet", true, true, false},
    {"reply shown, share sheet", true, true, true},
};

TEST_F(LinkPagesLayoutTest, TheMainDevicesPageHasOnePrimaryButtonAndACentredReplyCodeInEveryState) {
  for (const char* lang : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(lang);
    for (const WindowSize& size : kWindowSizes) {
      const bool compact = size.compact;
      const int window_width = size.width;
      const int width = compact ? window_width : window_width - 340; // the desktop's detail pane beside the list
      const int height = size.height;
      for (const MainCase& state_case : kMainCases) {
        const std::string where = std::string(lang) + (compact ? " phone " : " desktop ") +
                                  std::to_string(window_width) + "x" + std::to_string(height) + " " + state_case.name;
        const QrDisplay display =
            LinkQrDisplayFor(compact, static_cast<float>(window_width), static_cast<float>(height), 1.f,
                             kLinkQrDesktopSettingsReservedWidthDp);
        const std::string minutes = "15";
        Vars vars{{"security_can_export_link", "1"},
                  {"own_devices_not_main", "0"},
                  {"link_reply_shown", state_case.reply_shown ? "1" : "0"},
                  {"link_can_scan", state_case.can_scan ? "1" : "0"},
                  {"link_can_share", state_case.can_share ? "1" : "0"},
                  {"move_valid_label", Tr("settings.security.link_device.copied_valid", {{"minutes", minutes}})},
                  {"link_qr_side", display.css},
                  {"link_reply_qr_rml", "x"}};
        const std::string page = Bind(ReadAll(AssetPath("views/settings_security_transfer.rml")), vars);
        const std::string wrapper =
            compact ? "<div class=\"shell-layer shell-layer-compact\">"
                      "<div class=\"settings-subpage-view\" style=\"position: absolute; top: 0; "
                      "left: 0; width: " +
                          std::to_string(width) + "dp; height: " + std::to_string(height) + "dp; overflow-y: auto;\">"
                    : "<div class=\"settings-detail-view\" style=\"position: absolute; top: 0; "
                      "left: 0; width: " +
                          std::to_string(width) + "dp; height: " + std::to_string(height) + "dp; overflow-y: auto;\">";
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
          // Done is the one main button; Copy (and Send) are the quiet ones, in a row above it, left to right.
          std::vector<ui::Element*> quiet;
          ui::Element* done = nullptr;
          for (ui::Element* button : Visible("settings-link-btn")) {
            if (button->IsClassSet("btn-primary")) {
              done = button;
            } else {
              quiet.push_back(button);
            }
          }
          std::sort(quiet.begin(), quiet.end(),
                    [](ui::Element* a, ui::Element* b) { return Box(a).left < Box(b).left; });
          ASSERT_NE(done, nullptr) << where;
          ASSERT_EQ(quiet.size(), state_case.can_share ? 2U : 1U) << where << ": copy and send (if any), then done";
          EXPECT_TRUE(Visible("settings-transfer-btn").empty()) << where;
          EXPECT_TRUE(Visible("settings-transfer-steps").empty()) << where << ": no step list over the reply";
          EXPECT_EQ(Visible("settings-step-num").size(), 1U) << where << ": step 3 is the box's title";
          ui::Element* note = One("settings-link-note");
          ASSERT_NE(note, nullptr) << where;
          ExpectTargetsAndNoOverlap({One("settings-copied-title"), qr, One("settings-copied-sub"), note}, where);
          ExpectInside(box, {qr, note, One("link-step")}, where);
          EXPECT_GE(Box(done).top, Box(quiet.front()).bottom) << where << ": done below";
          if (state_case.can_share) {
            ExpectOneRow(quiet[0], quiet[1], where);
            EXPECT_NEAR((Box(quiet[0]).left + Box(quiet[1]).right) / 2.f, Box(box).MidX(), 1.f) << where;
          } else {
            EXPECT_NEAR(Box(quiet[0]).MidX(), Box(box).MidX(), 1.f) << where << ": copy is centred";
          }
        } else {
          const auto buttons = Visible("settings-transfer-btn");
          ASSERT_EQ(buttons.size(), state_case.can_scan ? 2U : 1U) << where << ": scan and paste, or paste alone";
          EXPECT_EQ(Visible("settings-step-num").size(), 3U) << where << ": the three steps are listed";
          EXPECT_TRUE(Visible("settings-link-qr").empty()) << where;
          ExpectTargetsAndNoOverlap({One("settings-transfer-steps")}, where);
          ExpectInside(One("settings-transfer-copy"), Visible("link-step"), where);
          if (buttons.size() == 2) {
            ExpectOneRow(buttons[0], buttons[1], where);
          } else {
            EXPECT_GE(Box(buttons[0]).bottom - Box(buttons[0]).top, 44.f) << where;
          }
          ExpectInside(One("settings-transfer-copy"), {}, where);
          for (ui::Element* button : buttons) {
            EXPECT_LE(Box(button).bottom - Box(button).top, 56.f) << where << ": one line";
          }
          EXPECT_TRUE(buttons[0]->IsClassSet("btn-primary")) << where << ": the first button is the main one";
        }
      }
    }
  }
}

// The main device's page in the settings pane of a desktop window: the pane is the window less the title bar (36dp) and
// the status bar (24dp) and the section list (340dp). Every shown element of the page lies inside that pane (the
// pane may scroll; what is asked is that a normal window does not need it).
TEST_F(LinkPagesLayoutTest, TheMainDevicesPageOnAComputerFitsTheSettingsPane) {
  for (const char* lang : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(lang);
    for (const DesktopWindow& window : kDesktopWindows) {
      if (window.width < 600) {
        continue; // a narrow window is the phone's layout (the test above)
      }
      const int pane_width = window.width - 340;
      const int pane_height = window.height - 60;
      for (const MainCase& state_case : kMainCases) {
        const std::string where = std::string(lang) + " desktop pane " + std::to_string(pane_width) + "x" +
                                  std::to_string(pane_height) + " " + state_case.name;
        const QrDisplay display =
            LinkQrDisplayFor(false, static_cast<float>(window.width), static_cast<float>(pane_height + 60), 1.f,
                             kLinkQrDesktopSettingsReservedWidthDp);
        Vars vars{{"security_can_export_link", "1"},
                  {"own_devices_not_main", "0"},
                  {"link_reply_shown", state_case.reply_shown ? "1" : "0"},
                  {"link_can_scan", state_case.can_scan ? "1" : "0"},
                  {"link_can_share", state_case.can_share ? "1" : "0"},
                  {"move_valid_label", Tr("settings.security.link_device.copied_valid", {{"minutes", "15"}})},
                  {"link_qr_side", display.css},
                  {"link_reply_qr_rml", "x"}};
        const std::string page = Bind(ReadAll(AssetPath("views/settings_security_transfer.rml")), vars);
        Mount("<div class=\"settings-detail-view\" style=\"position: absolute; top: 0; left: 0; width: " +
                  std::to_string(pane_width) + "dp; height: " + std::to_string(pane_height) +
                  "dp; overflow-y: auto;\">" + page + "</div>",
              pane_width, pane_height);
        ASSERT_NE(document, nullptr) << where;
        ui::Element* pane = One("settings-detail-view");
        ASSERT_NE(pane, nullptr) << where;
        const bool fits = !state_case.reply_shown || window.height >= 700;
        std::vector<ui::Element*> shown;
        CollectShown(pane, shown);
        for (ui::Element* e : shown) {
          const Edges box = Box(e);
          const std::string who = where + ": " + std::string(e->GetTagName().c_str()) + "." +
                                  std::string(e->GetClassNames().c_str());
          EXPECT_LE(box.right, static_cast<float>(pane_width) + 0.5f) << who << " is right of the pane";
          if (fits) {
            EXPECT_LE(box.bottom, static_cast<float>(pane_height) + 0.5f) << who << " is below the pane";
          } else {
            // A short window: the reply (a 280dp code and three 50dp buttons) is taller than the pane even at the
            // smallest code, so the settings pane scrolls, as every settings page does; all of it is reachable.
            EXPECT_LE(box.bottom, pane->GetScrollHeight() + 0.5f) << who << " is outside the scrollable pane";
          }
        }
        if (fits) {
          EXPECT_LE(pane->GetScrollHeight(), pane->GetClientHeight() + 1.f) << where << ": the pane needs scrolling";
        }
      }
    }
  }
}

} // namespace
} // namespace pbr
