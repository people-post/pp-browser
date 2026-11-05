#pragma once

#include <ui/base/Types.h>

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pbr {

/**
 * A phone "pick one value" settings page (Kenneth 2026-11-04, ui-mockups/09-sheets-unify ①): instead of a menu
 * from the bottom, the row pushes a page with one row per option and a check on the current one. A tap applies the
 * option at once through the same code the old menu ran, moves the check, and the page goes back after a short beat
 * (kPopDelayMs) so the user sees the check land. No DOM here: SettingsController binds it and pushes the page.
 */
class SettingsChoicePage {
public:
  struct Option {
    ui::String id;
    ui::String label;
    bool selected = false;
  };

  static constexpr int kPopDelayMs = 250;

  /** `apply` runs with the chosen option's id; `help_topic` is the "?" setting key ("" when the row has none). */
  void Open(std::string kind, std::string title, std::string foot, std::string help_topic, std::vector<Option> options,
            std::function<void(const std::string&)> apply) {
    kind_ = std::move(kind);
    title_ = std::move(title);
    foot_ = std::move(foot);
    help_topic_ = std::move(help_topic);
    options_ = std::move(options);
    apply_ = std::move(apply);
    picked_ = false;
  }

  void Close() {
    kind_.clear();
    options_.clear();
    apply_ = {};
    picked_ = false;
  }

  bool IsOpen() const { return !kind_.empty(); }

  /**
   * A row was tapped: the check moves to it and `apply` runs (also when it was already the current value: nothing
   * changes, and the page still goes back). False for an unknown id or a second tap while the page is leaving.
   */
  bool Pick(const std::string& id) {
    if (!IsOpen() || picked_) {
      return false;
    }
    const auto found = std::find_if(options_.begin(), options_.end(),
                                    [&id](const Option& option) { return id == option.id.c_str(); });
    if (found == options_.end()) {
      return false;
    }
    for (Option& option : options_) {
      option.selected = &option == &*found;
    }
    picked_ = true;
    if (apply_) {
      const auto apply = apply_; // may reopen or close this page
      apply(id);
    }
    return true;
  }

  /** A pick happened: the page goes back once the beat is over. */
  bool Picked() const { return picked_; }

  const std::string& Kind() const { return kind_; }
  const std::string& Title() const { return title_; }
  const std::string& Foot() const { return foot_; }
  const std::string& HelpTopic() const { return help_topic_; }
  std::vector<Option>& Options() { return options_; }
  const std::vector<Option>& Options() const { return options_; }

private:
  std::string kind_;
  std::string title_;
  std::string foot_;
  std::string help_topic_;
  std::vector<Option> options_;
  std::function<void(const std::string&)> apply_;
  bool picked_ = false;
};

} // namespace pbr
