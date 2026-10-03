#pragma once

#include "common/chat/PeopleDiscoveryBlocks.h"
#include "domain/ai/ArticleFeedBlocks.h"
#include "foundation/i18n/LocalizationService.h"

#include <string>

namespace pbr {

/** Tr(key), or `fallback` when the catalog has no entry (Tr echoes the key; e.g. no locale loaded in tests). */
inline std::string TrOrDefault(const std::string& key, const std::string& fallback) {
  std::string text = Tr(key);
  return text == key ? fallback : text;
}

/** People-discovery block labels in the UI language. Only for blocks shown to the user, never for LLM text. */
inline PeopleDiscoveryLabels LocalizedPeopleDiscoveryLabels() {
  const PeopleDiscoveryLabels d;
  PeopleDiscoveryLabels l;
  l.message = TrOrDefault("people.result.message", d.message);
  l.view = TrOrDefault("people.result.view", d.view);
  l.add_contact = TrOrDefault("people.result.add_contact", d.add_contact);
  l.in_contacts = TrOrDefault("people.result.in_contacts", d.in_contacts);
  l.unknown_person = TrOrDefault("people.result.unknown_person", d.unknown_person);
  l.start_chat_with = TrOrDefault("people.result.start_chat_with", d.start_chat_with);
  l.show_name = TrOrDefault("people.result.show_name", d.show_name);
  l.show_ids_for = TrOrDefault("people.result.show_ids_for", d.show_ids_for);
  l.add_name = TrOrDefault("people.result.add_name", d.add_name);
  l.refine_label = TrOrDefault("people.result.refine_label", d.refine_label);
  l.refine_message = TrOrDefault("people.result.refine_message", d.refine_message);
  l.found_one = TrOrDefault("people.result.found_one", d.found_one);
  l.found_many_partial = TrOrDefault("people.result.found_many_partial", d.found_many_partial);
  l.found_many = TrOrDefault("people.result.found_many", d.found_many);
  l.results_title = TrOrDefault("people.result.results_title", d.results_title);
  l.results_title_partial = TrOrDefault("people.result.results_title_partial", d.results_title_partial);
  l.local_one = TrOrDefault("people.result.local_one", d.local_one);
  l.local_many_partial = TrOrDefault("people.result.local_many_partial", d.local_many_partial);
  l.local_many = TrOrDefault("people.result.local_many", d.local_many);
  l.contacts_title = TrOrDefault("people.result.contacts_title", d.contacts_title);
  l.no_people = TrOrDefault("people.result.no_people", d.no_people);
  return l;
}

/** Article feed block labels in the UI language. Only for blocks shown to the user, never for LLM text. */
inline ArticleFeedLabels LocalizedArticleFeedLabels() {
  const ArticleFeedLabels d;
  ArticleFeedLabels l;
  l.intro = TrOrDefault("feed.result.intro", d.intro);
  l.title = TrOrDefault("feed.result.title", d.title);
  l.empty = TrOrDefault("feed.result.empty", d.empty);
  l.open = TrOrDefault("feed.result.open", d.open);
  l.more = TrOrDefault("feed.result.more", d.more);
  l.more_message = TrOrDefault("feed.result.more_message", d.more_message);
  return l;
}

} // namespace pbr
