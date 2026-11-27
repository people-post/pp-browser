#pragma once

#include <string>

namespace pbr {

/**
 * A sentence that names someone ("Drop to send to {name}", "发给 {name}") cut at the name, so a view can set the name in
 * its own weight whatever side of the sentence it falls on in the language. `prefix` and `suffix` are what is before and
 * after it.
 */
struct NameWords {
  std::string prefix;
  std::string name;
  std::string suffix;
};

/** The marker a caller puts where the name goes when it asks the translator for the sentence (never in a real text). */
inline constexpr char kNameMarker = '\x1f';

/** `sentence` (made with `kNameMarker` as the name) cut at the marker; no marker: all of it is the prefix. */
inline NameWords SplitAtNameMarker(const std::string& sentence, const std::string& name) {
  NameWords words;
  const size_t at = sentence.find(kNameMarker);
  if (at == std::string::npos) {
    words.prefix = sentence;
    return words;
  }
  words.prefix = sentence.substr(0, at);
  words.name = name;
  words.suffix = sentence.substr(at + 1);
  return words;
}

} // namespace pbr
