#pragma once

#include "lib/ai/AppActionClassifier.h"
#include "lib/ai/TurnPlan.h"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace pbr {

/**
 * Turns a sentence the wordlist has classified as "operate the app" (AppActionClassifier.h) into a TurnPlan
 * without a model: the tool comes from the wordlist, the parameters are read from the sentence by fixed rules
 * (LocalActionWords.h). Anything unclear is asked back, never guessed. Pure functions, no I/O.
 *
 * The rules live in DEV/PLAN-local-app-actions.md, section A.
 */

/** A local contact as the planner sees it (the caller flattens its own contact type into this). */
struct LocalPlanContact {
  std::string id;
  std::string thread_id;
  std::string remark;   // what the user typed (local.display_name); never leaves the device
  std::string nickname; // what the other side calls itself
  int64_t last_contact_ms = 0;
};

struct LocalPlanContext {
  std::vector<LocalPlanContact> contacts;
  /** Shipped UI locales as (tag, label), from list_locales. Empty: "zh-Hans" / "en" are assumed. */
  std::vector<std::pair<std::string, std::string>> locales;
};

/** A parameter is missing or unclear: ask `question_key` (a locale key), the user's next sentence answers it. */
struct LocalAsk {
  std::string tool;
  std::string missing; // query | nickname | enabled | appearance | language | policy | capability | contact
  std::string question_key;
  /**
   * set_mesh_capabilities only, what the sentence already said: the capability name when missing == "enabled",
   * "true" / "false" when missing == "capability".
   */
  std::string carry;
};

/** Several contacts fit (or none: `candidates` empty = not found). `identical_rows`: two rows read the same. */
struct LocalAmbiguous {
  std::string tool;
  std::vector<LocalPlanContact> candidates;
  bool identical_rows = false;
};

/** The tool is in the wordlist but cannot be run locally (add_contact has no tool; unknown names). */
struct LocalUnsupported {
  std::string tool;
};

/** A question about the app, not a command: hand it to the streamed answer. */
struct LocalQuestion {};

/** The user answered a question with "never mind". */
struct LocalCancelled {};

using LocalPlan = std::variant<TurnPlan, LocalAsk, LocalAmbiguous, LocalUnsupported, LocalQuestion, LocalCancelled>;

/**
 * `match` is the classifier's result for `text`. TurnPlan::synthesis_hints is kHintOnlyFirstAction when the
 * sentence had a second action behind a stop word that was not taken. A match without spans (brief_AI handed
 * the turn back and the wordlist did not hit) takes no parameters: the tool's Ask, or its plan if it has none.
 */
LocalPlan BuildLocalTurnPlan(const AppActionMatch& match, const std::string& text, const LocalPlanContext& ctx);

/**
 * The next sentence after an Ask: read it for `ask.missing` by that parameter's rule. Returns the TurnPlan, the
 * same Ask again (could not read it), LocalCancelled (short answer with a cancel word) or LocalQuestion (a how/why
 * question — also for a text parameter, "这个怎么用？" is not a nickname).
 */
LocalPlan ResolvePendingAnswer(const LocalAsk& ask, const std::string& answer, const LocalPlanContext& ctx);

/** The Ask a tool starts with when nothing was taken from the sentence (or its plan, if it needs no parameter). */
LocalPlan AskForTool(const std::string& tool, const std::string& text);

/** Is `text` a how/why question rather than a command? `settings_read`: "是什么" does not count. */
bool IsLocalActionQuestion(const std::string& text, bool settings_read = false);

} // namespace pbr
