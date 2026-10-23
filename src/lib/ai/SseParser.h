#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pbr {

struct SseEvent {
  std::string event; // "message" when the stream gives no event: field
  std::string data;  // data: lines joined with '\n'
};

// Incremental Server-Sent-Events parser (WHATWG event-stream rules). Bytes pass through untouched;
// events are emitted only when complete, so multi-byte UTF-8 split across chunks stays intact.
class SseParser {
public:
  // Feed raw bytes as they arrive; completed events are appended to `out`.
  // Returns false once the buffered partial line exceeds the size limit (caller should abort).
  bool Feed(std::string_view chunk, std::vector<SseEvent>& out);

private:
  void ProcessLine(std::vector<SseEvent>& out);

  std::string line_;
  std::string event_;
  std::string data_;
  bool has_data_ = false;
  bool skip_lf_ = false; // previous byte was '\r'; swallow an immediately following '\n'
  bool first_line_ = true;
};

} // namespace pbr
