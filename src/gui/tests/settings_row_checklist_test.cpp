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

std::string View(const std::string& id) {
  return ReadAsset("views/settings_section_" + id + ".rml");
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
  std::string gates; // enclosing data-if expressions, outermost first, joined by ";;"; "-" for none
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
};

std::vector<std::string> StackGates(const std::vector<Open>& stack) {
  std::vector<std::string> gates;
  for (const Open& open : stack) {
    if (!open.data_if.empty()) {
      gates.push_back(open.data_if);
    }
  }
  return gates;
}

// Every token (see the regex) with the data-if chain around it. Mirrors the script that wrote the checklist.
std::vector<Occurrence> Scan(const std::string& raw) {
  static const std::regex token_re(
      R"((\{\{[^}]*\}\}|data-event-[a-z]+="[^"]*"|data-value="[^"]*"|data-class-settings-switch--on="[^"]*"|data-help-key="[^"]*"))");
  const std::string text = StripComments(raw);
  std::vector<Occurrence> found;
  std::vector<Open> stack;
  const auto add_tokens = [&](const std::string& segment, const std::vector<std::string>& gates) {
    for (std::sregex_iterator it(segment.begin(), segment.end(), token_re), end; it != end; ++it) {
      found.push_back({(*it)[1].str(), JoinGates(gates)});
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
    add_tokens(text.substr(cur, at - cur), StackGates(stack));
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
      add_tokens(tag, gates);
      if (tag.size() < 2 || tag.compare(tag.size() - 2, 2, "/>") != 0) {
        stack.push_back(open);
      }
    }
    cur = end;
    at = end;
  }
  add_tokens(text.substr(cur), StackGates(stack));
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

} // namespace pbr
