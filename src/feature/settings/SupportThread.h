#pragma once

#include "common/Error.h"
#include "common/thread/ThreadRecordTypes.h"
#include "lib/net/ClientCompat.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * The PP 客服 chat (Application's EnsureSupportThread): the pure parts, so they can be tested without the app. The
 * chat is found or made without being opened; the settings "?" panel shows it over the page the user is on.
 */

/** PP 客服 is configured, enabled and has an account; otherwise an error with the user's words ("暂时联系不上"). */
Roe<void> CheckSupportConfigured(const std::optional<ClientCompatSupport>& discovery);

/** `error` with words for the user: its own when it has them, else the "暂时联系不上 PP 客服" sentence. */
Error WithSupportWords(Error error);

/** The PP 客服 chat among `threads`: the direct public chat with `contact_id`; none when it was never made. */
std::optional<std::string> FindSupportThread(const std::vector<Thread>& threads, const std::string& contact_id);

} // namespace pbr
