#include "feature/presenters/NewsReader.h"

#include <optional>

namespace pbr {

namespace {

bool IsUtmParameter(std::string_view parameter) {
  if (parameter.size() < 4) {
    return false;
  }
  for (size_t i = 0; i < 4; ++i) {
    const char c =
        parameter[i] >= 'A' && parameter[i] <= 'Z' ? static_cast<char>(parameter[i] - 'A' + 'a') : parameter[i];
    if (c != "utm_"[i]) {
      return false;
    }
  }
  return true;
}

/** A request's URL: its first line, trimmed. */
std::string RequestUrl(const std::string& text) {
  std::string_view line(text);
  line = line.substr(0, line.find('\n'));
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
    line.remove_prefix(1);
  }
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
    line.remove_suffix(1);
  }
  return std::string(line);
}

} // namespace

std::string NormalizeNewsUrl(std::string_view url) {
  // A hash route ("#/a/1", "#!/a/1") names the page; any other fragment is a place on it.
  std::string fragment;
  if (const size_t hash = url.find('#'); hash != std::string_view::npos) {
    const std::string_view after = url.substr(hash + 1);
    if (!after.empty() && (after.front() == '/' || after.front() == '!')) {
      fragment = "#" + std::string(after);
    }
    url = url.substr(0, hash);
  }
  const size_t query = url.find('?');
  if (query == std::string_view::npos) {
    return std::string(url) + fragment;
  }
  std::string out(url.substr(0, query));
  std::string kept;
  std::string_view rest = url.substr(query + 1);
  while (!rest.empty()) {
    const size_t amp = rest.find('&');
    const std::string_view parameter = rest.substr(0, amp);
    if (!parameter.empty() && !IsUtmParameter(parameter)) {
      kept += kept.empty() ? "" : "&";
      kept += parameter;
    }
    if (amp == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(amp + 1);
  }
  if (!kept.empty()) {
    out += "?" + kept;
  }
  return out + fragment;
}

std::string NewsReadRequestText(const std::string& url, const std::string& ui_language) {
  const bool chinese = ui_language.rfind("zh", 0) == 0;
  return url + "\n" + (chinese ? "这篇讲了什么" : "What's in the article");
}

namespace {

/** One line without its Markdown marks: heading / quote / list markers, emphasis, code ticks; a link keeps its label. */
std::string StripMarkdownLine(std::string_view line) {
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '#' || line.front() == '>')) {
    line.remove_prefix(1);
  }
  if (line.size() >= 2 && (line.front() == '-' || line.front() == '*' || line.front() == '+') && line[1] == ' ') {
    line.remove_prefix(2);
  } else {
    size_t digits = 0;
    while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') {
      ++digits;
    }
    if (digits > 0 && digits + 1 < line.size() && line[digits] == '.' && line[digits + 1] == ' ') {
      line.remove_prefix(digits + 2);
    }
  }
  std::string out;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '*' || c == '_' || c == '`') {
      continue;
    }
    if (c == '[') {
      const size_t close = line.find("](", i);
      const size_t end = close == std::string_view::npos ? close : line.find(')', close);
      if (end != std::string_view::npos) {
        out += StripMarkdownLine(line.substr(i + 1, close - i - 1));
        i = end;
        continue;
      }
    }
    out += c;
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t' || out.back() == '\r')) {
    out.pop_back();
  }
  return out;
}

} // namespace

std::string NewsPickTitle(const std::string& pick_text, const std::string& url) {
  std::string_view rest(pick_text);
  while (!rest.empty()) {
    const size_t newline = rest.find('\n');
    const std::string_view line = rest.substr(0, newline);
    rest = newline == std::string_view::npos ? std::string_view() : rest.substr(newline + 1);
    // The details link itself is no headline.
    if (line.find(std::string_view("\"pp:news\"")) != std::string_view::npos) {
      continue;
    }
    if (std::string title = StripMarkdownLine(line); !title.empty()) {
      return title;
    }
  }
  std::string_view host(url);
  if (const size_t scheme = host.find("://"); scheme != std::string_view::npos) {
    host.remove_prefix(scheme + 3);
  }
  return std::string(host.substr(0, host.find_first_of("/?#")));
}

const NewsReads::Digest* NewsReads::Find(const std::string& url) const {
  const auto it = digests.find(NormalizeNewsUrl(url));
  return it == digests.end() ? nullptr : &it->second;
}

NewsReads ScanNewsReads(const std::vector<ThreadMessage>& oldest_first) {
  NewsReads reads;
  std::optional<std::string> asked; // the URL of the request still waiting for its answer
  for (const ThreadMessage& message : oldest_first) {
    if (!message.generation) {
      continue;
    }
    const std::string& generation = *message.generation;
    if (generation == kNewsRequestGeneration && message.sender_contact_id == kLocalSelfContactId) {
      asked = NormalizeNewsUrl(RequestUrl(message.text));
    } else if (IsNewsDigestMessage(message) && message.sender_contact_id == kAiAssistantContactId && asked) {
      reads.digests[*asked] =
          NewsReads::Digest{message.id, message.text, generation == kNewsDigestTranslatedGeneration};
      asked.reset();
    } else if (generation == kNewsFailedGeneration && message.sender_contact_id == kAiAssistantContactId) {
      asked.reset();
    }
  }
  return reads;
}

const ThreadMessage* NewestShownMessage(const std::vector<ThreadMessage>& oldest_first) {
  for (auto it = oldest_first.rbegin(); it != oldest_first.rend(); ++it) {
    if (!IsHiddenNewsMessage(*it)) {
      return &*it;
    }
  }
  return nullptr;
}

NewsTap DecideNewsTap(const bool stored, const bool reading_this, const bool busy) {
  if (stored) {
    return NewsTap::ShowStored;
  }
  if (reading_this) {
    return NewsTap::ShowReading;
  }
  return busy ? NewsTap::Wait : NewsTap::StartRead;
}

NewsTap DecideNewsTapFor(const NewsReads& reads, const std::string& item_url, const std::string& reading_url,
                         const bool busy) {
  const bool reading_this = !reading_url.empty() && NormalizeNewsUrl(reading_url) == NormalizeNewsUrl(item_url);
  return DecideNewsTap(reads.IsRead(item_url), reading_this, busy);
}

NewsReaderShows NewsReaderShowsFor(const NewsReads& reads, const std::string& item_url, const std::string& reading_url,
                                   const std::string_view streamed) {
  NewsReaderShows shows;
  shows.digest = reads.Find(item_url);
  if (shows.digest) {
    shows.translated = shows.digest->translated;
  } else if (!reading_url.empty() && NormalizeNewsUrl(reading_url) == NormalizeNewsUrl(item_url)) {
    shows.reading = true;
    shows.partial = streamed;
    shows.loading = streamed.empty();
  } else {
    shows.failed = true;
    shows.can_retry = true;
  }
  return shows;
}

bool NewsRetryTakesTap(const std::optional<std::chrono::steady_clock::time_point> failed_since,
                       const std::chrono::steady_clock::time_point now) {
  return failed_since && now - *failed_since >= kNewsRetryGuard;
}

NewsNav NewsNavFor(const size_t index, const size_t count, const bool reading) {
  NewsNav nav;
  nav.shown = count >= 2;
  nav.can_prev = nav.shown && !reading && index > 0;
  nav.can_next = nav.shown && !reading && index + 1 < count;
  return nav;
}

} // namespace pbr
