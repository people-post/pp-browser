#include "foundation/diagnostics/DiagnosticsBundle.h"

#include "common/ValueJson.h"
#include "foundation/diagnostics/DiagnosticsSelfCheck.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace pbr {

namespace {

constexpr std::uintmax_t kMaxReadBytes = 8u * 1024u * 1024u;
constexpr std::string_view kCutMarker = " \xE2\x80\xA6[cut]";  // " …[cut]"

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

/** True for "[YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] [Channel] " with LEVEL in INFO/WARNING/ERROR and an allowed channel. */
bool HasAllowedPrefix(std::string_view line) {
  // "[2026-10-28 08:57:32.329]" is 25 bytes.
  static constexpr std::string_view kShape = "[dddd-dd-dd dd:dd:dd.ddd]";
  if (line.size() < kShape.size() + 1) return false;
  for (std::size_t i = 0; i < kShape.size(); ++i) {
    if (kShape[i] == 'd' ? !IsDigit(line[i]) : line[i] != kShape[i]) return false;
  }
  std::size_t pos = kShape.size();
  if (line.compare(pos, 2, " [") != 0) return false;
  pos += 2;
  const std::size_t level_end = line.find(']', pos);
  if (level_end == std::string_view::npos) return false;
  const std::string_view level = line.substr(pos, level_end - pos);
  if (level != "INFO" && level != "WARNING" && level != "ERROR" && level != "CRITICAL") return false;
  pos = level_end + 1;
  if (line.compare(pos, 2, " [") != 0) return false;
  pos += 2;
  const std::size_t chan_end = line.find(']', pos);
  if (chan_end == std::string_view::npos) return false;
  const std::string_view channel = line.substr(pos, chan_end - pos);
  if (line.compare(chan_end + 1, 1, " ") != 0) return false;
  return std::find(std::begin(kDiagnosticsChannels), std::end(kDiagnosticsChannels), channel) !=
         std::end(kDiagnosticsChannels);
}

/** Replace bytes that are not valid UTF-8 with '?', so the JSON writer never rejects a line. */
std::string SanitizeUtf8(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  std::size_t i = 0;
  while (i < in.size()) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    std::size_t len = 0;
    if (c < 0x80) len = 1;
    else if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c >= 0xE0 && c <= 0xEF) len = 3;
    else if (c >= 0xF0 && c <= 0xF4) len = 4;
    bool ok = len != 0 && i + len <= in.size();
    for (std::size_t k = 1; ok && k < len; ++k) {
      ok = (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
    }
    if (ok) {
      out.append(in.substr(i, len));
      i += len;
    } else {
      out.push_back('?');
      ++i;
    }
  }
  return out;
}

std::string CutLong(std::string line) {
  if (line.size() <= kDiagnosticsMaxLineBytes) return line;
  std::size_t keep = kDiagnosticsMaxLineBytes - kCutMarker.size();
  while (keep > 0 && (static_cast<unsigned char>(line[keep]) & 0xC0) == 0x80) --keep;  // not mid-character
  line.resize(keep);
  line.append(kCutMarker);
  return line;
}

/** The lines that pass the prefix filter, keeping file order (oldest first). */
void Filter(const std::vector<std::string>& raw, std::vector<std::string>& out) {
  for (const std::string& line : raw) {
    if (!HasAllowedPrefix(line)) continue;
    out.push_back(CutLong(SanitizeUtf8(line)));
  }
}

std::vector<std::string> ReadTailLines(const std::filesystem::path& path) {
  std::vector<std::string> lines;
  if (path.empty()) return lines;
  std::ifstream in(path, std::ios::binary);
  if (!in) return lines;
  in.seekg(0, std::ios::end);
  const std::uintmax_t size = static_cast<std::uintmax_t>(in.tellg());
  const bool truncated = size > kMaxReadBytes;
  in.seekg(truncated ? static_cast<std::streamoff>(size - kMaxReadBytes) : 0, std::ios::beg);
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::size_t pos = 0;
  if (truncated) {  // first line is partial
    const std::size_t nl = data.find('\n');
    pos = nl == std::string::npos ? data.size() : nl + 1;
  }
  while (pos < data.size()) {
    std::size_t nl = data.find('\n', pos);
    if (nl == std::string::npos) nl = data.size();
    std::size_t end = nl;
    if (end > pos && data[end - 1] == '\r') --end;
    lines.emplace_back(data, pos, end - pos);
    pos = nl + 1;
  }
  return lines;
}

struct Section {
  std::string name;
  std::vector<std::string> lines;  // oldest first
  int drop_rank = 0;               // lower is dropped first when over the cap
  std::size_t first = 0;           // lines before `first` are dropped
  std::size_t Count() const { return lines.size() - first; }
};

std::size_t LineCost(const std::string& line) {
  return DumpJson(Value(line)).size() + 1;  // + comma
}

std::string Serialize(const DiagnosticsBundleInput& in, const std::vector<Section>& sections) {
  Object root;
  root.set("schema_version", static_cast<int64_t>(1));
  root.set("client_token", in.client_token);
  root.set("product", in.product);
  root.set("version", in.version);
  root.set("os", in.os);
  root.set("captured_at", in.captured_at_ms);
  std::vector<Value> files;
  for (const Section& s : sections) {
    if (s.Count() == 0) continue;
    Object file;
    file.set("name", s.name);
    std::vector<Value> lines;
    lines.reserve(s.Count());
    for (std::size_t i = s.first; i < s.lines.size(); ++i) lines.emplace_back(s.lines[i]);
    file.set("lines", makeArray(std::move(lines)));
    files.emplace_back(ObjectValue(std::move(file)));
  }
  if (files.empty()) {  // keep at least one file entry
    Object file;
    file.set("name", sections.empty() ? std::string("diag.log") : sections.front().name);
    file.set("lines", makeArray({}));
    files.emplace_back(ObjectValue(std::move(file)));
  }
  root.set("files", makeArray(std::move(files)));
  return DumpJson(root);
}

} // namespace

std::vector<std::string> CrashEnvelopeLines(const CrashReportEnvelope& envelope, std::string_view timestamp) {
  const std::string prefix = "[" + std::string(timestamp) + "] [INFO] [Crash] ";
  auto one_line = [](const std::string& v) {
    std::string out;
    for (char c : v) {
      if (c == '\n') out += "\\n";
      else if (c == '\r') out += "\\r";
      else out.push_back(c);
    }
    return out;
  };
  std::vector<std::string> lines;
  lines.push_back(prefix + "reason=" + one_line(envelope.reason));
  for (const std::string& f : envelope.frames) lines.push_back(prefix + "frame=" + one_line(f));
  for (const std::string& t : envelope.breadcrumb_tags) lines.push_back(prefix + "tag=" + one_line(t));
  return lines;
}

Roe<DiagnosticsBundle> BuildDiagnosticsBundle(const DiagnosticsBundleInput& input) {
  if (input.files.size() + 1 > kDiagnosticsMaxFiles) {
    return Error("bundle: more files than an upload takes");
  }
  std::vector<Section> sections;

  // Rank: the first file is dropped last; the crash record outlives them all.
  for (const auto& path : input.files) {
    Section s;
    s.name = path.filename().string();
    s.drop_rank = static_cast<int>(input.files.size() - sections.size());
    Filter(ReadTailLines(path), s.lines);
    sections.push_back(std::move(s));
  }
  {
    Section s;
    s.name = "crash";
    s.drop_rank = static_cast<int>(input.files.size()) + 1;
    Filter(input.crash_lines, s.lines);
    sections.push_back(std::move(s));
  }

  // Cap: drop the oldest lines first, from the least wanted file first (the last file given ... the first, crash).
  auto drop_one = [&sections]() -> bool {
    Section* victim = nullptr;
    for (Section& s : sections) {
      if (s.Count() > 0 && (victim == nullptr || s.drop_rank < victim->drop_rank)) victim = &s;
    }
    if (victim == nullptr) return false;
    ++victim->first;
    return true;
  };
  auto total_lines = [&sections]() {
    std::size_t n = 0;
    for (const Section& s : sections) n += s.Count();
    return n;
  };
  while (total_lines() > static_cast<std::size_t>(kDiagnosticsMaxLines) && drop_one()) {
  }
  // Budget: fixed part (header + empty file entries) plus the escaped size of each kept line; the dump below
  // is re-measured and trimmed further if the estimate was a few bytes short.
  std::vector<Section> empty_shape;
  empty_shape.reserve(sections.size());
  for (const Section& s : sections) {
    Section header;
    header.name = s.name;
    empty_shape.push_back(std::move(header));
  }
  std::size_t bytes = Serialize(input, empty_shape).size();
  for (const Section& s : sections) {
    for (std::size_t i = s.first; i < s.lines.size(); ++i) bytes += LineCost(s.lines[i]);
  }
  while (bytes > kDiagnosticsMaxJsonBytes) {
    Section* victim = nullptr;
    for (Section& s : sections) {
      if (s.Count() > 0 && (victim == nullptr || s.drop_rank < victim->drop_rank)) victim = &s;
    }
    if (victim == nullptr) break;
    bytes -= std::min(bytes, LineCost(victim->lines[victim->first]));
    ++victim->first;
  }

  // Self-check over exactly what would be sent.
  std::string all_text;
  for (const Section& s : sections) {
    for (std::size_t i = s.first; i < s.lines.size(); ++i) {
      all_text += s.lines[i];
      all_text.push_back('\n');
    }
  }
  if (auto hit = SelfCheck(all_text, input.known_identifiers)) {
    return Error("self-check: " + *hit);
  }

  DiagnosticsBundle bundle;
  bundle.json = Serialize(input, sections);
  while (bundle.json.size() > kDiagnosticsMaxJsonBytes && drop_one()) {
    bundle.json = Serialize(input, sections);
  }
  if (bundle.json.find("json_encode_failed") == 0 || bundle.json.rfind("{\"error\"", 0) == 0) {
    return Error("bundle: json encode failed");
  }
  for (const Section& s : sections) bundle.lines += static_cast<int>(s.Count());
  bundle.bytes = bundle.json.size();
  return bundle;
}

} // namespace pbr
