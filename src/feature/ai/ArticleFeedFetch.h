#pragma once

#include "common/Error.h"
#include "common/Value.h"
#include "foundation/data/Config.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** Points McpClient's HTTP transport at the app's HttpClient (the same for every caller; safe to call again). */
void WireMcpHttpPost();

/**
 * Calls the article feed tool (`blog_articles`) of the promoted MCP server once, outside any AI turn: the server and
 * tool the "Latest briefs" chip reaches, on a short-lived client of its own (Home's Today's briefs widget,
 * DESIGN-pp-ui 8.16). `arguments` as SuggestionPayload builds them. Blocking: call it on a worker.
 * Returns the feed document ({"articles":[...]}); an error when there is no HTTP server or it does not answer.
 */
Roe<std::string> FetchArticleFeed(const AppConfig& config, const Object& arguments);

} // namespace pbr
