#include "domain/ai/SseParser.h"

#include "common/PlatformLimits.h"

namespace pbr {

bool SseParser::Feed(std::string_view chunk, std::vector<SseEvent>& out) {
  for (const char c : chunk) {
    if (skip_lf_) {
      skip_lf_ = false;
      if (c == '\n') {
        continue;
      }
    }
    if (c == '\n' || c == '\r') {
      skip_lf_ = (c == '\r');
      ProcessLine(out);
      line_.clear();
      continue;
    }
    line_.push_back(c);
    if (line_.size() > kMaxLlmResponseBytes) {
      return false;
    }
  }
  return true;
}

void SseParser::ProcessLine(std::vector<SseEvent>& out) {
  if (first_line_) {
    first_line_ = false;
    if (line_.compare(0, 3, "\xEF\xBB\xBF") == 0) {
      line_.erase(0, 3);
    }
  }

  if (line_.empty()) {
    if (has_data_) {
      out.push_back(SseEvent{event_.empty() ? "message" : std::move(event_), std::move(data_)});
    }
    event_.clear();
    data_.clear();
    has_data_ = false;
    return;
  }
  if (line_[0] == ':') {
    return;
  }

  const size_t colon = line_.find(':');
  const std::string_view field = std::string_view(line_).substr(0, colon);
  std::string_view value;
  if (colon != std::string::npos) {
    value = std::string_view(line_).substr(colon + 1);
    if (!value.empty() && value.front() == ' ') {
      value.remove_prefix(1);
    }
  }

  if (field == "event") {
    event_ = value;
  } else if (field == "data") {
    if (has_data_) {
      data_ += '\n';
    }
    data_ += value;
    has_data_ = true;
  }
}

} // namespace pbr
