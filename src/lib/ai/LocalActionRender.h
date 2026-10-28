#pragma once

#include "lib/ai/LocalActionPlanner.h"
#include "common/PbrCompat.h"

#include <string>

namespace pbr {

/**
 * Fixed-template rendering of the on-device "operate the app" flow (DEV/PLAN-local-app-actions.md, section C).
 * Questions, results, candidate lists: all text comes from the locale files, the only variable parts are values
 * from tool results or the local context. No model text, no model call. Pure functions apart from Tr().
 */

struct LocalRenderContext {
  /** open_conversation / start_conversation: the contact's effective title (remark first). UI text only, never sent to a model. */
  std::string name;
  /** Arguments the tool ran with: a placeholder the result does not carry (e.g. the new nickname) is read from here. */
  Object args;
  /** The plan's synthesis_hints; kHintOnlyFirstAction appends the "I did only the first thing" sentence. */
  std::string synthesis_hints;
};

/** The question for `ask.question_key` (set_reduce_transparency etc. name the setting itself). */
std::string RenderLocalAsk(const LocalAsk& ask);

/**
 * `result_json` is the tool's JSON result. A failure ({"success": false, …}; a Roe error is passed that way) never
 * shows the tool's own text: {"blocked": true} (the user's tool permissions) → ai.blocked_by_permissions, anything
 * else → ai.action_failed. Readers (get_* / list_locales) become a "label: value" card, one line per key; a key
 * without an `ai.card.<tool>.<key>` label is left out (logged once), or shown as "??key??" under
 * SetLocalRenderStrictForTests(true) so the registry-driven tests catch it. Writers become one sentence.
 * list_* / search_people results are not handled here (people-list blocks): returns "".
 */
std::string RenderLocalResult(const std::string& tool, const Object& result_json, const LocalRenderContext& ctx);

/** Tests: an unlabelled card key renders as "??key??" instead of being skipped. */
void SetLocalRenderStrictForTests(bool strict);

/**
 * People-list blocks JSON for contacts the user must choose from. Rows carry {"type":"local_action",...} payloads:
 * open_conversation (thread_id + contact_id) / start_conversation (contact_id) per row; when `identical_rows`,
 * edit_remark (contact_id) and no open action. No candidates: a "not found" paragraph and a row that carries
 * {"type":"local_action","tool":"search_people","query":<query>}.
 */
std::string RenderLocalAmbiguous(const LocalAmbiguous& ambiguous, const std::string& query = {});

/** A short "how do I ask for this" answer for a how/why question about `tool`; ai.cannot_do_that_yet without a key. */
std::string RenderLocalHowTo(const std::string& tool);
/** Does `tool` have an `ai.howto.<tool>` text? */
bool HasLocalHowTo(const std::string& tool);

std::string RenderLocalCancelled();
std::string RenderLocalGaveUp();
std::string RenderLocalUnsupported();
std::string RenderLocalNotShareable();
std::string RenderLocalContactGone();

} // namespace pbr
