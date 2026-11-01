#pragma once

namespace pbr {

/** What "Add contact" does with a single directory hit. */
enum class AddFromSearchDecision {
  AlreadyContact, // say so and select them; change nothing
  ConfirmByName,  // found by name: ask before adding
  AddDirect,      // an exact id was typed: add straight away
};

inline AddFromSearchDecision DecideAddFromSearch(const bool query_is_exact_id, const bool is_known_contact) {
  if (is_known_contact) {
    return AddFromSearchDecision::AlreadyContact;
  }
  return query_is_exact_id ? AddFromSearchDecision::AddDirect : AddFromSearchDecision::ConfirmByName;
}

} // namespace pbr
