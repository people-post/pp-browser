// The settings reorganisation moves rows between pages without changing any of them (DEV/PLAN-settings-reorg.md).
// This test keeps that promise honest: data/settings_row_checklist.txt lists, per page view, every event, binding,
// switch expression and label key with how often it appears and under which `data-if` chain; the views must match it
// exactly, and what moved out of a view that is edited in place must be gone from it.
//
// Phases: a row tagged "N" in the last column may change its gate from phase N on (the plan's phase table);
// kPhase is the phase the tree is at.
#include "feature/settings/SettingsPages.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int kPhase = 2;

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string text = buffer.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

std::string ReadAsset(const std::string& relative) {
  return ReadFile(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
}

std::string ReadSource(const std::string& relative) {
  return ReadFile(std::filesystem::path(PP_SOURCE_DIR) / relative);
}

// A page view with its {{include:x}} sub-views spelled out in place (the desktop PIN and the transfer block live in
// their own files), so the checks below see everything the page can show.
//
// The Advanced page's sub-pages (its free-text fields on phones) are listed under their own ids; the page itself does not
// spell out the files it includes for the desktop, so no row is counted twice.
const std::vector<std::string>& AdvancedSubpages() {
  static const std::vector<std::string> ids = {"advanced_endpoint", "advanced_integrations", "advanced_backend"};
  return ids;
}

std::string View(const std::string& id) {
  std::string text = ReadAsset("views/settings_section_" + id + ".rml");
  const std::string marker = "{{include:";
  for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker)) {
    const size_t end = text.find("}}", at);
    const std::string name = text.substr(at + marker.size(), end - at - marker.size());
    const bool subpage = name.rfind("settings_section_advanced_", 0) == 0;
    text.replace(at, end + 2 - at, subpage ? std::string() : ReadAsset("views/" + name + ".rml"));
  }
  return text;
}

std::string StripComments(const std::string& text) {
  std::string out;
  size_t at = 0;
  while (at < text.size()) {
    const size_t open = text.find("<!--", at);
    if (open == std::string::npos) {
      out.append(text, at, std::string::npos);
      break;
    }
    out.append(text, at, open - at);
    const size_t close = text.find("-->", open);
    if (close == std::string::npos) {
      break;
    }
    at = close + 3;
  }
  return out;
}

struct Occurrence {
  std::string token;
  std::string gates;   // enclosing data-if expressions, outermost first, joined by ";;"; "-" for none
  std::string layouts; // where it can show, from the settings-phone-only / settings-desktop-only classes around it
};

std::string JoinGates(const std::vector<std::string>& gates) {
  if (gates.empty()) {
    return "-";
  }
  std::string out;
  for (const std::string& gate : gates) {
    out += out.empty() ? gate : ";;" + gate;
  }
  return out;
}

struct Open {
  std::string data_if;
  bool phone_only = false;   // class settings-phone-only: hidden on desktop
  bool desktop_only = false; // class settings-desktop-only: hidden on phones
};

std::string LayoutsOf(const std::vector<Open>& stack, const Open* self) {
  bool phone_only = self && self->phone_only;
  bool desktop_only = self && self->desktop_only;
  for (const Open& open : stack) {
    phone_only = phone_only || open.phone_only;
    desktop_only = desktop_only || open.desktop_only;
  }
  if (phone_only && desktop_only) {
    return "none";
  }
  return phone_only ? "phone" : desktop_only ? "desktop" : "both";
}

std::vector<std::string> StackGates(const std::vector<Open>& stack) {
  std::vector<std::string> gates;
  for (const Open& open : stack) {
    if (!open.data_if.empty()) {
      gates.push_back(open.data_if);
    }
  }
  return gates;
}

bool HasClass(const std::string& tag, const std::string& cls) {
  const size_t at = tag.find("class=\"");
  if (at == std::string::npos) {
    return false;
  }
  const size_t start = at + 7;
  const std::string value = " " + tag.substr(start, tag.find('"', start) - start) + " ";
  return value.find(" " + cls + " ") != std::string::npos;
}

// Every token (see the regex) with the data-if chain around it. Mirrors the script that wrote the checklist.
std::vector<Occurrence> Scan(const std::string& raw) {
  static const std::regex token_re(
      R"((\{\{[^}]*\}\}|data-event-[a-z]+="[^"]*"|data-value="[^"]*"|data-class-settings-switch--on="[^"]*"|data-help-key="[^"]*"))");
  const std::string text = StripComments(raw);
  std::vector<Occurrence> found;
  std::vector<Open> stack;
  const auto add_tokens = [&](const std::string& segment, const std::vector<std::string>& gates,
                              const std::string& layouts) {
    for (std::sregex_iterator it(segment.begin(), segment.end(), token_re), end; it != end; ++it) {
      found.push_back({(*it)[1].str(), JoinGates(gates), layouts});
    }
  };
  size_t cur = 0;
  size_t at = 0;
  while (at < text.size()) {
    if (text[at] != '<') {
      ++at;
      continue;
    }
    size_t end = at + 1;
    bool quoted = false;
    while (end < text.size()) {
      const char c = text[end];
      if (c == '"') {
        quoted = !quoted;
      } else if (c == '>' && !quoted) {
        break;
      }
      ++end;
    }
    ++end;
    add_tokens(text.substr(cur, at - cur), StackGates(stack), LayoutsOf(stack, nullptr));
    const std::string tag = text.substr(at, end - at);
    if (tag.compare(0, 2, "</") == 0) {
      if (!stack.empty()) {
        stack.pop_back();
      }
    } else {
      Open open;
      for (size_t pos = tag.find("data-if=\""); pos != std::string::npos; pos = tag.find("data-if=\"", pos + 1)) {
        if (pos > 0 && std::isspace(static_cast<unsigned char>(tag[pos - 1]))) {
          const size_t value = pos + 9;
          open.data_if = tag.substr(value, tag.find('"', value) - value);
          break;
        }
      }
      std::vector<std::string> gates = StackGates(stack);
      if (!open.data_if.empty()) {
        gates.push_back(open.data_if);
      }
      open.phone_only = HasClass(tag, "settings-phone-only");
      open.desktop_only = HasClass(tag, "settings-desktop-only");
      add_tokens(tag, gates, LayoutsOf(stack, &open));
      if (tag.size() < 2 || tag.compare(tag.size() - 2, 2, "/>") != 0) {
        stack.push_back(open);
      }
    }
    cur = end;
    at = end;
  }
  add_tokens(text.substr(cur), StackGates(stack), LayoutsOf(stack, nullptr));
  EXPECT_TRUE(stack.empty()) << "unbalanced tags";
  return found;
}

// The gates around the place a sub-page file is included in a view, so a sub-page's rows carry their full chain (the page's
// own `advanced` gate and the include site's wrappers, then the file's own conditions). `view` is the including file.
std::string IncludeSiteGates(const std::string& view, const std::string& subpage_id) {
  std::string text = ReadAsset("views/" + view + ".rml");
  const std::string marker = "{{include:";
  const std::string target = "settings_section_" + subpage_id;
  size_t found = 0;
  for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at)) {
    const size_t end = text.find("}}", at);
    const std::string name = text.substr(at + marker.size(), end - at - marker.size());
    if (name == target) {
      text.replace(at, end + 2 - at, "{{SITE}}");
      ++found;
    } else {
      text.replace(at, end + 2 - at, std::string());
    }
  }
  std::string gates = "-";
  size_t sites = 0;
  for (const Occurrence& occurrence : Scan(text)) {
    if (occurrence.token == "{{SITE}}") {
      gates = occurrence.gates;
      ++sites;
    }
  }
  EXPECT_EQ(found, 1u) << view << " must include " << target << " exactly once";
  EXPECT_EQ(sites, 1u);
  return gates;
}

std::string JoinChain(const std::string& outer, const std::string& inner) {
  if (outer == "-") {
    return inner;
  }
  return inner == "-" ? outer : outer + ";;" + inner;
}

// The occurrences of a page's rows. A sub-page of Advanced is scanned in its file and then placed in the chain of its
// desktop include site in the Advanced page, so losing the `advanced` gate there, or a condition of its own, is a diff.
std::vector<Occurrence> ScanPage(const std::string& page_id) {
  std::vector<Occurrence> found = Scan(View(page_id));
  const std::vector<std::string>& subpages = AdvancedSubpages();
  if (std::find(subpages.begin(), subpages.end(), page_id) != subpages.end()) {
    const std::string site = IncludeSiteGates("settings_section_advanced", page_id);
    for (Occurrence& occurrence : found) {
      occurrence.gates = JoinChain(site, occurrence.gates);
    }
  }
  return found;
}

size_t CountOf(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

struct Row {
  std::string token;
  size_t count = 0;
  std::string handler;
  std::string view;
  std::string gates;
  std::vector<std::string> from_views;
  int phase = 0; // 0: the gate may not change
};

std::vector<std::string> Split(const std::string& line, char sep) {
  std::vector<std::string> fields;
  size_t start = 0;
  for (size_t at = line.find(sep); at != std::string::npos; at = line.find(sep, start)) {
    fields.push_back(line.substr(start, at - start));
    start = at + 1;
  }
  fields.push_back(line.substr(start));
  return fields;
}

std::vector<Row> LoadChecklist() {
  std::vector<Row> rows;
  std::istringstream in(ReadSource("src/gui/tests/data/settings_row_checklist.txt"));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const std::vector<std::string> f = Split(line, '\t');
    if (f.size() != 7) {
      ADD_FAILURE() << "bad checklist line: " << line;
      continue;
    }
    Row row;
    row.token = f[0];
    row.count = std::stoul(f[1]);
    row.handler = f[2];
    row.view = f[3];
    row.gates = f[4];
    row.from_views = Split(f[5], ',');
    row.phase = f[6].empty() ? 0 : std::stoi(f[6]);
    rows.push_back(std::move(row));
  }
  return rows;
}

const std::set<std::string>& InPlaceViews() {
  static const std::set<std::string> views = {"profile", "security", "appearance", "storage"};
  return views;
}

const std::vector<std::string>& RetiredViews() {
  static const std::vector<std::string> views = {"llm", "integrations", "network", "about"};
  return views;
}

} // namespace

TEST(SettingsRowChecklistTest, EveryPageIsRegisteredAndTheRetiredViewsAreNot) {
  const std::string catalog = ReadSource("src/lib/ui/ViewCatalog.cpp");
  const std::string sections = ReadAsset("views/settings_sections.rml");
  std::set<std::string> ids;
  for (const SettingsPage& page : SettingsPages()) {
    EXPECT_TRUE(ids.insert(page.id).second) << "duplicate page id " << page.id;
    EXPECT_EQ(std::string(page.view_key), std::string("settings_section_") + page.id);
    EXPECT_NE(catalog.find(std::string("{\"") + page.view_key + "\", \"views/" + page.view_key + ".rml\"}"),
              std::string::npos)
        << page.view_key << " is not in ViewCatalog";
    EXPECT_NE(sections.find(std::string("{{include:") + page.view_key + "}}"), std::string::npos) << page.view_key;
    EXPECT_FALSE(View(page.id).empty()) << page.view_key << " has no file";
  }
  for (const std::string& id : RetiredViews()) {
    EXPECT_EQ(catalog.find("settings_section_" + id), std::string::npos) << id << " is still in ViewCatalog";
    EXPECT_EQ(sections.find("settings_section_" + id), std::string::npos) << id << " is still included";
    EXPECT_EQ(ids.count(id), 0u) << id << " is still a page";
  }
}

TEST(SettingsRowChecklistTest, HomeEntriesAreAllInThePageTable) {
  const std::string home = ReadAsset("views/settings.rml");
  const std::regex select_re(R"(select_section\('([a-z_]+)'\))");
  for (std::sregex_iterator it(home.begin(), home.end(), select_re), end; it != end; ++it) {
    EXPECT_NE(FindSettingsPage((*it)[1].str()), nullptr) << "Home opens a page that is not in the table";
  }
  // The phone list is the table: one data-for per group.
  for (int group = 1; group <= 3; ++group) {
    EXPECT_EQ(CountOf(home, "data-if=\"section.group == " + std::to_string(group) + "\""), 1u) << group;
  }
  size_t listed = 0;
  for (const SettingsPage& page : SettingsPages()) {
    listed += page.home_group >= 1 && page.home_group <= 3 ? 1 : 0;
    EXPECT_GE(page.home_group, 0);
    EXPECT_LE(page.home_group, 3);
  }
  EXPECT_EQ(listed, SettingsPages().size() - 1) << "everything but the profile card is a list entry";
}

TEST(SettingsRowChecklistTest, EveryRowOfEveryPageIsOnTheChecklistWithItsGates) {
  const std::vector<Row> rows = LoadChecklist();
  ASSERT_GT(rows.size(), 100u);
  std::vector<std::string> ids;
  for (const SettingsPage& page : SettingsPages()) {
    ids.push_back(page.id);
  }
  ids.insert(ids.end(), AdvancedSubpages().begin(), AdvancedSubpages().end());
  for (const std::string& page_id : ids) {
    std::map<std::string, size_t> actual_total;
    std::map<std::pair<std::string, std::string>, size_t> actual_at;
    for (const Occurrence& occurrence : ScanPage(page_id)) {
      ++actual_total[occurrence.token];
      ++actual_at[{occurrence.token, occurrence.gates}];
    }
    std::map<std::string, size_t> expected_total;
    for (const Row& row : rows) {
      if (row.view != page_id) {
        continue;
      }
      expected_total[row.token] += row.count;
      const bool gate_may_differ = row.phase != 0 && kPhase >= row.phase;
      if (!gate_may_differ) {
        const std::pair<std::string, std::string> key{row.token, row.gates};
        EXPECT_EQ(actual_at[key], row.count) << page_id << ": " << row.token << " under [" << row.gates << "]";
      }
    }
    EXPECT_EQ(actual_total, expected_total) << page_id << ": the view and the checklist disagree on what is in it";
  }
}

TEST(SettingsRowChecklistTest, WhatMovedOutOfAViewIsGoneFromIt) {
  const std::vector<Row> rows = LoadChecklist();
  std::map<std::pair<std::string, std::string>, size_t> stays; // (view, token) -> how often the checklist keeps it
  for (const Row& row : rows) {
    stays[{row.view, row.token}] += row.count;
  }
  size_t checked = 0;
  for (const Row& row : rows) {
    for (const std::string& from : row.from_views) {
      if (from == row.view || InPlaceViews().count(from) == 0) {
        continue;
      }
      const size_t kept = stays[{from, row.token}];
      EXPECT_EQ(CountOf(StripComments(View(from)), row.token), kept)
          << row.token << " must not be left in " << from << " (it moved to " << row.view << ")";
      ++checked;
    }
  }
  EXPECT_GT(checked, 50u);
  // The old views and the holding files of phase 0a are deleted.
  for (const std::string& id : RetiredViews()) {
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" /
                                         ("settings_section_" + id + ".rml")))
        << id;
  }
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / "holding"));
}

// The sensitive actions of Security and Devices, row by row: each event is carried by exactly one element per layout
// (phone / desktop), under exactly the gate chain it had before the mockup rebuild (90a581e9b). The PIN gate itself
// lives in the action handlers; this pins that no other event, copy or gate reached them.
TEST(SettingsSensitiveActionsTest, EachEventSitsOnOneElementPerLayoutUnderItsOldGates) {
  struct Expect {
    const char* page;
    const char* event;
    const char* gates;
    size_t phone;   // visible on a phone
    size_t desktop; // visible on the desktop
  };
  const Expect expected[] = {
      {"security", "open_security_page('change_pin')", "show_change_pin", 1, 0},
      {"security", "change_pin()", "show_change_pin && security_can_change_pin", 0, 1},
      {"security", "toggle_biometric()", "security_biometric_shown", 1, 1},
      {"security", "on_choose_unlock_grace()", "security_grace_shown", 1, 1},
      {"security", "show_recovery_phrase()", "security_has_recovery_phrase;;recovery_phrase == ''", 1, 1},
      {"security", "hide_recovery_phrase()", "security_has_recovery_phrase;;recovery_phrase != ''", 1, 1},
      {"security", "ask_setting_help('settings.security.recovery')", "security_has_recovery_phrase", 1, 1},
      // Devices: the transfer block is the desktop's inline copy here; phones open it as its own page.
      // (2026-11-14: the phone's top button, on the main device only; a linked phone shows nothing to tap.)
      {"devices", "open_security_page('transfer')", "!own_devices_not_main", 1, 0},
      // The one "?" of the action, next to the top sentence, on the main device only.
      {"devices", "ask_setting_help('settings.security.transfer_row')", "!own_devices_not_main", 1, 0},
      // Here the phone's button and "Copy again" sit in the transfer block's desktop wrapper (hidden): only the desktop
      // button shows. Their own page is checked in the next test.
      {"devices", "export_link_device()",
       "security_can_export_link && !own_devices_not_main@@security_can_export_link && "
       "!own_devices_not_main;;!transfer_code_copied@@security_can_export_link && "
       "!own_devices_not_main;;transfer_code_copied",
       0, 1},
      // The phone's device rows moved into a sheet (open_own_device_actions); its two actions run the same Remove /
      // MakeMain the desktop buttons' events run (SettingsOwnDevicesTest). The desktop keeps one element per event.
      {"devices", "make_main_device(device_index)", "own_devices_shown;;device.can_manage", 0, 1},
      {"devices", "remove_own_device(device_index)", "own_devices_shown;;device.can_manage", 0, 1},
      {"devices", "open_own_device_actions(device_index)",
       "own_devices_shown && !own_devices_not_main;;device.can_manage", 1, 0},
      {"devices", "save_account_copy()", "security_has_recovery_phrase", 1, 1},
      {"devices", "ask_setting_help('settings.help.topic.account_copy')", "security_has_recovery_phrase", 1, 1},
  };
  for (const Expect& e : expected) {
    const std::string token = std::string("data-event-click=\"") + e.event + "\"";
    size_t phone = 0;
    size_t desktop = 0;
    for (const Occurrence& o : Scan(View(e.page))) {
      if (o.token != token) {
        continue;
      }
      // "a@@b": the element may sit under either gate (two variants of one row).
      const std::vector<std::string> alternatives = Split(e.gates, '@');
      EXPECT_TRUE(std::find_if(alternatives.begin(), alternatives.end(),
                               [&](const std::string& gate) { return gate == o.gates; }) != alternatives.end())
          << e.page << ": " << e.event << " under [" << o.gates << "]";
      phone += (o.layouts == "phone" || o.layouts == "both") ? 1 : 0;
      desktop += (o.layouts == "desktop" || o.layouts == "both") ? 1 : 0;
    }
    EXPECT_EQ(phone, e.phone) << e.page << " (phone): " << e.event;
    EXPECT_EQ(desktop, e.desktop) << e.page << " (desktop): " << e.event;
  }
}

TEST(SettingsSensitiveActionsTest, TheTransferBlockOnItsOwnPageFiresTheOneExportEventUnderTheOldGate) {
  // settings.rml includes the same file for the phone's own transfer page. The phone has the primary button and, in the
  // "copied" box, "Copy again": both fire export_link_device() under the old gate (the box only adds the view flag);
  // the desktop keeps its one button.
  const std::string rml = ReadAsset("views/settings_security_transfer.rml");
  const std::string gate = "security_can_export_link && !own_devices_not_main";
  size_t phone = 0;
  size_t desktop = 0;
  for (const Occurrence& o : Scan(rml)) {
    if (o.token == "data-event-click=\"export_link_device()\"") {
      EXPECT_EQ(o.gates.rfind(gate, 0), 0u) << "the old gate comes first: " << o.gates;
      if (o.layouts == "desktop") {
        EXPECT_EQ(o.gates, gate);
      } else {
        EXPECT_TRUE(o.gates == gate + ";;!transfer_code_copied" || o.gates == gate + ";;transfer_code_copied")
            << o.gates;
      }
      phone += (o.layouts == "phone" || o.layouts == "both") ? 1 : 0;
      desktop += (o.layouts == "desktop" || o.layouts == "both") ? 1 : 0;
    }
  }
  EXPECT_EQ(phone, 2u);
  EXPECT_EQ(desktop, 1u);
}

TEST(SettingsSensitiveActionsTest, NoHelpButtonSitsInsideAnotherButton) {
  // Two nested buttons fire both events on one tap; the "?" must be a sibling of the row's own button.
  for (const char* page : {"security", "devices"}) {
    const std::string text = StripComments(View(page));
    int depth = 0;
    for (size_t at = text.find('<'); at != std::string::npos; at = text.find('<', at + 1)) {
      if (text.compare(at, 7, "<button") == 0) {
        ++depth;
        EXPECT_LE(depth, 1) << page << ": a button inside a button";
      } else if (text.compare(at, 9, "</button>") == 0) {
        --depth;
      }
    }
  }
}

// --- one caption per group on phones (Kenneth 2026-11-14) ---------------------------------------------------------

namespace {

// Evaluates a gate chain ("a;;!b && c") against a set of true facts: every chain element is an AND of (possibly negated)
// names, all of which must hold. Names outside the facts are false.
bool GateHolds(const std::string& chain, const std::set<std::string>& facts) {
  if (chain == "-") {
    return true;
  }
  std::vector<std::string> parts;
  for (size_t start = 0, at; start <= chain.size(); start = at + 2) {
    at = chain.find(";;", start);
    if (at == std::string::npos) {
      at = chain.size();
    }
    parts.push_back(chain.substr(start, at - start));
    if (at == chain.size()) {
      break;
    }
  }
  for (const std::string& part : parts) {
    for (size_t start = 0, at; start <= part.size(); start = at + 2) {
      at = part.find("&&", start);
      if (at == std::string::npos) {
        at = part.size();
      }
      std::string name = part.substr(start, at - start);
      name.erase(0, name.find_first_not_of(' '));
      name.erase(name.find_last_not_of(' ') + 1);
      const bool negated = !name.empty() && name[0] == '!';
      if (negated) {
        name.erase(0, 1);
      }
      if ((facts.count(name) > 0) == negated) {
        return false;
      }
      if (at == part.size()) {
        break;
      }
    }
  }
  return true;
}

// The phone-visible occurrences of the given tokens in the Devices page, under a state.
size_t PhoneShown(const std::vector<std::string>& tokens, const std::set<std::string>& facts) {
  size_t shown = 0;
  for (const Occurrence& o : Scan(View("devices"))) {
    if ((o.layouts == "phone" || o.layouts == "both") && GateHolds(o.gates, facts) &&
        std::find(tokens.begin(), tokens.end(), o.token) != tokens.end()) {
      ++shown;
    }
  }
  return shown;
}

} // namespace

namespace {

// The phone-visible tokens of the Devices page under a state.
std::set<std::string> PhoneTokens(const std::set<std::string>& facts) {
  std::set<std::string> shown;
  for (const Occurrence& o : Scan(View("devices"))) {
    if ((o.layouts == "phone" || o.layouts == "both") && GateHolds(o.gates, facts)) {
      shown.insert(o.token);
    }
  }
  return shown;
}

const std::set<std::string>& MainFacts() {
  static const std::set<std::string> facts = {"own_devices_shown", "device.can_manage", "security_can_export_link"};
  return facts;
}

const std::set<std::string>& LinkedFacts() {
  // Everything that could switch something on is true: the page must still offer nothing to tap.
  static const std::set<std::string> facts = {
      "own_devices_shown", "own_devices_not_main",     "own_has_other_devices",       "device.can_manage",
      "device.is_main",    "security_can_export_link", "security_has_recovery_phrase"};
  return facts;
}

const std::set<std::string>& SignedOutFacts() {
  static const std::set<std::string> facts = {"own_devices_shown", "own_devices_signed_out", "own_devices_not_main"};
  return facts;
}

} // namespace

TEST(SettingsDevicesCaptionsTest, ThePhoneShowsAtMostOneCaptionUnderTheDeviceList) {
  const std::vector<std::string> captions = {
      "{{i18n:settings.security.devices.help}}", "{{i18n:settings.security.devices.not_main}}",
      "{{i18n:settings.security.devices.tap_hint}}", "{{i18n:settings.security.devices.none}}",
      "{{i18n:settings.security.devices.signed_out}}"};
  std::set<std::string> main_with_others = {"own_devices_shown", "own_has_other_devices"};
  const std::set<std::string> main_alone = {"own_devices_shown"};
  EXPECT_EQ(PhoneShown(captions, main_with_others), 1u) << "main device with other devices: the tap hint";
  EXPECT_EQ(PhoneShown({"{{i18n:settings.security.devices.tap_hint}}"}, main_with_others), 1u);
  EXPECT_EQ(PhoneShown(captions, main_alone), 1u) << "main device alone: the grey line";
  EXPECT_EQ(PhoneShown({"{{i18n:settings.security.devices.none}}"}, main_alone), 1u);
  EXPECT_EQ(PhoneShown(captions, LinkedFacts()), 0u) << "a linked phone has its sentence card, no caption";
  EXPECT_EQ(PhoneShown(captions, SignedOutFacts()), 1u) << "signed-out phone: its notice only";
  EXPECT_EQ(PhoneShown({"{{i18n:settings.security.devices.signed_out}}"}, SignedOutFacts()), 1u);
}

TEST(SettingsDevicesCaptionsTest, TheDesktopKeepsItsCaptionsAsBefore) {
  size_t help = 0;
  size_t not_main = 0;
  for (const Occurrence& o : Scan(View("devices"))) {
    if (o.layouts == "desktop" && o.token == "{{i18n:settings.security.devices.help}}") {
      EXPECT_EQ(o.gates, "own_devices_shown");
      ++help;
    }
    if (o.layouts == "desktop" && o.token == "{{i18n:settings.security.devices.not_main}}") {
      EXPECT_EQ(o.gates, "own_devices_shown;;own_devices_not_main && !own_devices_signed_out");
      ++not_main;
    }
  }
  EXPECT_EQ(help, 1u);
  EXPECT_EQ(not_main, 1u);
}

TEST(SettingsDevicesCaptionsTest, ThePhoneShowsNoCaptionUnderTheAccountCopyRow) {
  // The "?" of the row explains it; the old captions are gone, and the desktop-only copy.help is not a phone caption
  // (the stylesheet hides it, see DevicesStyleTest).
  const std::string rml = StripComments(View("devices"));
  for (const char* gone : {"settings.devices.transfer_help", "settings.devices.actions_help",
                           "settings.security.transfer_off", "settings.security.devices.not_main_phone"}) {
    EXPECT_EQ(rml.find(gone), std::string::npos) << gone;
  }
  bool desktop_only = false;
  for (const Occurrence& o : Scan(View("devices"))) {
    if (o.token == "{{i18n:settings.security.copy.help}}") {
      desktop_only = o.layouts == "desktop";
    }
  }
  EXPECT_TRUE(desktop_only);
}

TEST(SettingsDevicesCaptionsTest, TheLinkButtonIsOnTheMainDeviceOnlyAndALinkedPhoneHasNothingToTap) {
  const std::string tap = "data-event-click=\"open_security_page('transfer')\"";
  const std::string help = "data-event-click=\"ask_setting_help('settings.security.transfer_row')\"";
  EXPECT_EQ(PhoneShown({tap}, MainFacts()), 1u);
  EXPECT_EQ(PhoneShown({help}, MainFacts()), 1u) << "one \"?\" for the action, in one place";
  // A linked phone: no element bound to any of the device actions, on the phone layout of this page or of the link page.
  for (const char* event : {"open_security_page('transfer')", "export_link_device()",
                            "open_own_device_actions(device_index)", "make_main_device(device_index)",
                            "remove_own_device(device_index)", "ask_setting_help('settings.security.transfer_row')"}) {
    const std::string token = std::string("data-event-click=\"") + event + "\"";
    EXPECT_EQ(PhoneShown({token}, LinkedFacts()), 0u) << "devices page, linked phone: " << event;
    for (const Occurrence& o : Scan(ReadAsset("views/settings_security_transfer.rml"))) {
      if (o.token == token && (o.layouts == "phone" || o.layouts == "both")) {
        EXPECT_FALSE(GateHolds(o.gates, LinkedFacts())) << "link page, linked phone: " << event;
      }
    }
  }
  // Only the account copy row at the bottom (and the "?" of it) is tappable on a linked phone.
  for (const std::string& token : PhoneTokens(LinkedFacts())) {
    const bool account_copy = token == "data-event-click=\"save_account_copy()\"" ||
                              token == "data-event-click=\"ask_setting_help('settings.help.topic.account_copy')\"";
    EXPECT_TRUE(account_copy || token.find("data-event-") == std::string::npos)
        << "a linked phone's list is read only: " << token;
  }
  EXPECT_EQ(PhoneShown({"{{i18n:settings.security.devices.linked_note}}"}, LinkedFacts()), 1u);
  EXPECT_EQ(PhoneShown({"{{i18n:settings.security.devices.hero}}"}, LinkedFacts()), 0u);
}

TEST(SettingsDevicesCaptionsTest, TheSignedOutPhoneShowsItsNoticeOnlyAndTheAccountCopyRowAsBefore) {
  EXPECT_EQ(PhoneTokens(SignedOutFacts()), std::set<std::string>({"{{i18n:settings.security.devices.signed_out}}"}));
  std::set<std::string> with_phrase = SignedOutFacts();
  with_phrase.insert("security_has_recovery_phrase");
  const std::set<std::string> shown = PhoneTokens(with_phrase);
  EXPECT_EQ(shown.count("data-event-click=\"save_account_copy()\""), 1u);
  EXPECT_EQ(shown.count("{{i18n:settings.security.devices.signed_out}}"), 1u);
  EXPECT_EQ(shown.size(), 4u) << "the notice, the row's title, its event and its \"?\"";
}

// --- the Advanced page's sub-pages keep their gates (Kenneth 2026-11-14) -------------------------------------------------

// The checklist rows of the three sub-page files carry the whole chain; this also names it, so an input that loses the
// `advanced` gate, or the custom service addresses their own condition, fails even if someone edits the checklist too.
TEST(SettingsAdvancedGatesTest, EveryInputOfASubpageSitsUnderAdvancedOnTheDesktopAndItsOwnConditionForCustomAddresses) {
  for (const std::string& id : AdvancedSubpages()) {
    size_t inputs = 0;
    for (const Occurrence& o : ScanPage(id)) {
      EXPECT_EQ(o.gates.rfind("advanced;;advanced", 0), 0u) << id << ": " << o.token << " under [" << o.gates << "]";
      if (id == "advanced_backend") {
        EXPECT_NE(o.gates.find(";;backend_is_custom"), std::string::npos) << o.token << " under [" << o.gates << "]";
      }
      inputs += o.token.rfind("data-value=", 0) == 0 ? 1 : 0;
    }
    EXPECT_GT(inputs, 0u) << id << " has inputs";
  }
}

TEST(SettingsAdvancedGatesTest, OnPhonesEachSubpageFileIsInItsOwnPageOfTheSubpageView) {
  const struct {
    const char* id;
    const char* page;
  } pages[] = {{"advanced_endpoint", "ai_endpoint"},
               {"advanced_integrations", "integrations"},
               {"advanced_backend", "backend_custom"}};
  for (const auto& p : pages) {
    EXPECT_EQ(IncludeSiteGates("settings", p.id),
              std::string("show_detail && security_page != '';;security_page == '") + p.page + "'")
        << p.id << ": the page opens only through open_security_page, which needs the Advanced page (controller guard)";
  }
}

// "Custom addresses" is reachable wherever the old inline fields were: the Advanced page with the service on Custom,
// whether or not the Service row is shown (the UI demo build hides it).
TEST(SettingsAdvancedGatesTest, TheCustomAddressesEntryDoesNotDependOnTheServiceRow) {
  const std::string entry = "data-event-click=\"open_security_page('backend_custom')\"";
  const auto shown = [&](const std::set<std::string>& facts) {
    size_t count = 0;
    for (const Occurrence& o : Scan(View("advanced"))) {
      if (o.token == entry && (o.layouts == "phone" || o.layouts == "both") && GateHolds(o.gates, facts)) {
        ++count;
      }
    }
    return count;
  };
  EXPECT_EQ(shown({"advanced", "backend_is_custom", "show_backend_switch"}), 1u) << "with the Service row";
  EXPECT_EQ(shown({"advanced", "backend_is_custom"}), 1u) << "without the Service row (demo build)";
  EXPECT_EQ(shown({"advanced", "show_backend_switch"}), 0u) << "the service is not Custom";
  EXPECT_EQ(shown({"advanced"}), 0u);
  EXPECT_EQ(shown({"backend_is_custom", "show_backend_switch"}), 0u) << "Advanced is off";
  EXPECT_EQ(shown({"backend_is_custom"}), 0u);
}

} // namespace pbr
