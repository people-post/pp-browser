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
std::string View(const std::string& id) {
  std::string text = ReadAsset("views/settings_section_" + id + ".rml");
  const std::string marker = "{{include:";
  for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker)) {
    const size_t end = text.find("}}", at);
    const std::string name = text.substr(at + marker.size(), end - at - marker.size());
    text.replace(at, end + 2 - at, ReadAsset("views/" + name + ".rml"));
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
  for (const SettingsPage& page : SettingsPages()) {
    std::map<std::string, size_t> actual_total;
    std::map<std::pair<std::string, std::string>, size_t> actual_at;
    for (const Occurrence& occurrence : Scan(View(page.id))) {
      ++actual_total[occurrence.token];
      ++actual_at[{occurrence.token, occurrence.gates}];
    }
    std::map<std::string, size_t> expected_total;
    for (const Row& row : rows) {
      if (row.view != page.id) {
        continue;
      }
      expected_total[row.token] += row.count;
      const bool gate_may_differ = row.phase != 0 && kPhase >= row.phase;
      if (!gate_may_differ) {
        const std::pair<std::string, std::string> key{row.token, row.gates};
        EXPECT_EQ(actual_at[key], row.count) << page.id << ": " << row.token << " under [" << row.gates << "]";
      }
    }
    EXPECT_EQ(actual_total, expected_total) << page.id << ": the view and the checklist disagree on what is in it";
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
      {"devices", "open_security_page('transfer')", "-", 1, 0},
      {"devices", "ask_setting_help('settings.security.transfer_row')", "-", 1, 0},
      {"devices", "export_link_device()", "security_can_export_link && !own_devices_not_main", 0, 1},
      // The phone's device rows moved into a sheet (open_own_device_actions); its two actions run the same Remove /
      // MakeMain the desktop buttons' events run (SettingsOwnDevicesTest). The desktop keeps one element per event.
      {"devices", "make_main_device(device_index)", "own_devices_shown;;device.can_manage", 0, 1},
      {"devices", "remove_own_device(device_index)", "own_devices_shown;;device.can_manage", 0, 1},
      {"devices", "open_own_device_actions(device_index)",
       "own_devices_shown;;own_has_other_devices;;device.can_manage", 1, 0},
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
      EXPECT_EQ(o.gates, e.gates) << e.page << ": " << e.event;
      phone += (o.layouts == "phone" || o.layouts == "both") ? 1 : 0;
      desktop += (o.layouts == "desktop" || o.layouts == "both") ? 1 : 0;
    }
    EXPECT_EQ(phone, e.phone) << e.page << " (phone): " << e.event;
    EXPECT_EQ(desktop, e.desktop) << e.page << " (desktop): " << e.event;
  }
}

TEST(SettingsSensitiveActionsTest, TheTransferBlockOnItsOwnPageShowsOneExportRowPerLayout) {
  // settings.rml includes the same file for the phone's own transfer page: there the phone row is the one that shows.
  const std::string rml = ReadAsset("views/settings_security_transfer.rml");
  size_t phone = 0;
  size_t desktop = 0;
  for (const Occurrence& o : Scan(rml)) {
    if (o.token == "data-event-click=\"export_link_device()\"") {
      EXPECT_EQ(o.gates, "security_can_export_link && !own_devices_not_main");
      phone += (o.layouts == "phone" || o.layouts == "both") ? 1 : 0;
      desktop += (o.layouts == "desktop" || o.layouts == "both") ? 1 : 0;
    }
  }
  EXPECT_EQ(phone, 1u);
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

} // namespace pbr
