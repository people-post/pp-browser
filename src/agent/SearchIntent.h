#pragma once

#include <string>

namespace pbr {

// True when the user message likely needs live/web data and search should run before the LLM answers.
bool ShouldProactiveWebSearch(const std::string& user_message);

// True when the user wants local contacts listed (list_contacts).
bool ShouldProactiveContactsList(const std::string& user_message);

// True when the user wants public directory search or local contacts without relying on LLM tool calls.
bool ShouldProactivePeopleDiscovery(const std::string& user_message);

// Convert conversational user text into a directory search query (may be empty = browse all).
std::string BuildPeopleSearchQuery(const std::string& user_message);

// True when the user wants an MCP article feed (e.g. brief.global blog_articles), not web_search headlines.
bool WantsArticleFeedRequest(const std::string& user_message);

// True when the user wants news headlines rather than a generic web lookup.
bool WantsNewsHeadlines(const std::string& user_message);

// Convert conversational user text into a concise web/news search query.
std::string BuildWebSearchQuery(const std::string& user_message);

} // namespace pbr
