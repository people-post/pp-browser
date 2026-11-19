#include "foundation/diagnostics/RunLogBundle.h"

#include "common/CivilTime.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>

namespace pbr {

namespace fs = std::filesystem;

namespace {

constexpr std::array<std::string_view, 9> kDropKeys = {"text",   "body",  "content", "query", "question",
                                                       "prompt", "draft", "caption", "raw"};
constexpr std::array<std::string_view, 8> kMaskKeys = {"name",     "display_name", "nickname", "title",
                                                       "filename", "path",         "subtitle", "user"};
constexpr std::string_view kHidden = "<hidden>";

bool IsKeyChar(const char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == '-';
}

bool IsKeyBoundary(const std::string_view text, const size_t at) {
  if (at == 0) {
    return true;
  }
  const char before = text[at - 1];
  return before == ' ' || before == '\t' || before == ',' || before == ';' || before == '(' || before == '[' ||
         before == '{' || before == '\n' || before == '?' || before == '&' || before == '"' || before == '\'' ||
         before == ':' || before == '/';
}

/** Where `key=` starts in `text` at or after `from`, on a key boundary; npos when nowhere. */
size_t FindKey(const std::string_view text, const std::string_view key, size_t from) {
  const std::string token = std::string(key) + "=";
  for (size_t at = text.find(token, from); at != std::string_view::npos; at = text.find(token, at + 1)) {
    if (IsKeyBoundary(text, at)) {
      return at;
    }
  }
  return std::string_view::npos;
}

/** Start of the next ` key=` token (a space or tab, then key characters, then `=`) at or after `from`; else npos. */
size_t NextKeyToken(const std::string_view text, size_t from) {
  for (size_t at = from; at < text.size(); ++at) {
    if (text[at] != ' ' && text[at] != '\t') {
      continue;
    }
    size_t end = at + 1;
    while (end < text.size() && IsKeyChar(text[end])) {
      ++end;
    }
    if (end > at + 1 && end < text.size() && text[end] == '=') {
      return at;
    }
  }
  return std::string_view::npos;
}

/** End of the value that starts at `value_at`: a quoted one at its closing quote (kept), else the next ` key=`. */
size_t ValueEnd(const std::string_view text, const size_t value_at) {
  if (value_at < text.size() && text[value_at] == '"') {
    for (size_t q = text.find('"', value_at + 1); q != std::string_view::npos; q = text.find('"', q + 1)) {
      const size_t after = q + 1;
      if (after == text.size() || NextKeyToken(text, after) == after) {
        return after;
      }
    }
    return text.size();
  }
  const size_t next = NextKeyToken(text, value_at);
  return next == std::string_view::npos ? text.size() : next;
}

std::string MaskValues(std::string record) {
  for (const std::string_view key : kMaskKeys) {
    for (size_t at = FindKey(record, key, 0); at != std::string::npos; at = FindKey(record, key, at + 1)) {
      const size_t value_at = at + key.size() + 1;
      const size_t end = ValueEnd(record, value_at);
      record.replace(value_at, end - value_at, kHidden);
    }
  }
  return record;
}

/** `prefix<segment>sep` → `~sep` (segment: a user or app folder name, spaces allowed, up to 64 bytes, on one line). */
void ReplacePathSegment(std::string& text, const std::string_view prefix, const char sep, const bool ignore_case) {
  auto find_from = [&](size_t from) {
    if (!ignore_case) {
      return text.find(prefix, from);
    }
    auto it = std::search(
        text.begin() + static_cast<std::ptrdiff_t>(from), text.end(), prefix.begin(), prefix.end(), [](char a, char b) {
          return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
    return it == text.end() ? std::string::npos : static_cast<size_t>(it - text.begin());
  };
  for (size_t at = find_from(0); at != std::string::npos; at = find_from(at + 1)) {
    constexpr size_t kMaxSegment = 64;
    size_t end = at + prefix.size();
    while (end < text.size() && end - at - prefix.size() <= kMaxSegment && text[end] != sep && text[end] != '\n' &&
           text[end] != '"') {
      ++end;
    }
    if (end == at + prefix.size() || end >= text.size() || text[end] != sep) {
      continue; // no segment, or not followed by the separator
    }
    text.replace(at, end + 1 - at, std::string("~") + sep);
  }
}

std::string ScrubPaths(std::string text) {
  // Longest first: the iOS container under /private, then the plain one.
  ReplacePathSegment(text, "/private/var/mobile/Containers/Data/Application/", '/', false);
  ReplacePathSegment(text, "/var/mobile/Containers/Data/Application/", '/', false);
  ReplacePathSegment(text, "/private/var/mobile/Containers/Shared/AppGroup/", '/', false);
  ReplacePathSegment(text, "/var/mobile/Containers/Shared/AppGroup/", '/', false);
  ReplacePathSegment(text, "/data/user/0/", '/', false);
  ReplacePathSegment(text, "/data/data/", '/', false);
  ReplacePathSegment(text, "/Users/", '/', false);
  ReplacePathSegment(text, "/home/", '/', false);
  for (const char drive : std::string_view("ABCDEFGHIJKLMNOPQRSTUVWXYZ")) {
    const std::string prefix = std::string(1, drive) + ":\\Users\\";
    ReplacePathSegment(text, prefix, '\\', true);
  }
  return text;
}

bool IsRecordHead(const std::string_view line) {
  // [YYYY-MM-DD hh:mm:ss
  static constexpr std::string_view kShape = "[0000-00-00 00:00:00";
  if (line.size() < kShape.size()) {
    return false;
  }
  for (size_t i = 0; i < kShape.size(); ++i) {
    const bool digit = std::isdigit(static_cast<unsigned char>(line[i])) != 0;
    if (kShape[i] == '0' ? !digit : line[i] != kShape[i]) {
      return false;
    }
  }
  return true;
}

std::string FilterRecord(const std::string_view record) {
  for (const std::string_view key : kDropKeys) {
    if (FindKey(record, key, 0) != std::string_view::npos) {
      return {};
    }
  }
  return ScrubPaths(MaskValues(std::string(record)));
}

/** The family a log file belongs to: its directory and stem without a rotation index (`pp-browser.3` → `pp-browser`). */
std::string FamilyOf(const std::string& name) {
  const fs::path p(name);
  std::string stem = p.stem().string();
  const size_t dot = stem.rfind('.');
  if (dot != std::string::npos && dot + 1 < stem.size() &&
      stem.find_first_not_of("0123456789", dot + 1) == std::string::npos) {
    stem.resize(dot);
  }
  return (p.parent_path() / (stem + p.extension().string())).string();
}

/** The last `take` bytes of `name` (as long as it was `size`), from its first to its last complete line. */
std::string ReadTail(const std::string& name, const std::uintmax_t size, const std::uintmax_t take) {
  std::ifstream in(name, std::ios::binary);
  if (!in) {
    return {};
  }
  const std::uintmax_t offset = take < size ? size - take : 0;
  in.seekg(static_cast<std::streamoff>(offset));
  std::string text(static_cast<size_t>(take), '\0');
  in.read(text.data(), static_cast<std::streamsize>(take));
  text.resize(static_cast<size_t>(in.gcount()));
  const size_t last = text.rfind('\n');
  text.resize(last == std::string::npos ? 0 : last + 1); // a line still being written is left out
  if (offset > 0) {
    const size_t first = text.find('\n');
    text.erase(0, first == std::string::npos ? text.size() : first + 1); // the cut line
  }
  return text;
}

std::string FirstRecordTime(const std::string_view text) {
  size_t at = 0;
  while (at < text.size()) {
    const size_t end = text.find('\n', at);
    const std::string_view line = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
    if (IsRecordHead(line)) {
      return std::string(line.substr(6, 5)) + " " + std::string(line.substr(12, 5)); // MM-DD hh:mm
    }
    if (end == std::string_view::npos) {
      break;
    }
    at = end + 1;
  }
  return {};
}

std::string LocalDate(const std::time_t now) {
  std::tm local{};
  if (!civil_time::LocalTime(now, &local)) {
    return "unknown-date";
  }
  char buffer[16] = {};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
  return buffer;
}

} // namespace

std::vector<LogFileInfo> RunLogBundle::DirectoryFiles(const std::string& log_path) {
  std::vector<LogFileInfo> files;
  if (log_path.empty()) {
    return files;
  }
  for (const std::string& family : {log_path, LogFile::MetricsPath(log_path), LogFile::DiagPath(log_path)}) {
    std::vector<LogFileInfo> part = LogFile::FamilyFiles(family);
    files.insert(files.end(), part.begin(), part.end());
  }
  return files;
}

bool RunLogBundle::Sendable(const std::vector<LogFileInfo>& main_family, const bool collect) {
  return collect ||
         std::any_of(main_family.begin(), main_family.end(), [](const LogFileInfo& file) { return !file.current; });
}

RunLogSelection RunLogBundle::Select(std::vector<LogFileInfo> files, const fs::file_time_type now,
                                     const LogRetention& retention, const std::uintmax_t size_limit) {
  std::map<std::string, std::vector<LogFileInfo>> families;
  for (LogFileInfo& file : files) {
    families[FamilyOf(file.name)].push_back(std::move(file));
  }
  std::vector<LogFileInfo> kept;
  for (auto& [family, members] : families) {
    const std::vector<std::string> expired = LogFile::ExpiredFiles(members, now, retention);
    for (LogFileInfo& member : members) {
      if (std::find(expired.begin(), expired.end(), member.name) == expired.end()) {
        kept.push_back(std::move(member));
      }
    }
  }
  std::stable_sort(kept.begin(), kept.end(), [](const LogFileInfo& a, const LogFileInfo& b) {
    return a.mtime > b.mtime; // newest first
  });

  RunLogSelection selection;
  std::uintmax_t left = size_limit;
  for (const LogFileInfo& file : kept) {
    if (left == 0) {
      selection.truncated = true;
      break;
    }
    RunLogPick pick{file.name, file.size, std::min(file.size, left)};
    if (pick.take_bytes < file.size) {
      selection.truncated = true;
    }
    left -= pick.take_bytes;
    selection.files.push_back(std::move(pick));
  }
  std::reverse(selection.files.begin(), selection.files.end()); // oldest first
  return selection;
}

std::string RunLogBundle::FilterRecords(const std::string_view text) {
  std::string out;
  std::string record;
  bool in_record = false;
  auto flush = [&]() {
    if (in_record && !record.empty()) {
      const bool newline = record.back() == '\n';
      if (newline) {
        record.pop_back();
      }
      std::string kept = FilterRecord(record);
      if (!kept.empty()) {
        out += kept;
        if (newline) {
          out += '\n';
        }
      }
    }
    record.clear();
  };
  size_t at = 0;
  while (at < text.size()) {
    const size_t end = text.find('\n', at);
    const size_t next = end == std::string_view::npos ? text.size() : end + 1;
    const std::string_view line = text.substr(at, next - at);
    if (IsRecordHead(line)) {
      flush();
      in_record = true;
    }
    if (in_record) {
      record += line; // a line before the first record belongs to none: dropped
    }
    at = next;
  }
  flush();
  return out;
}

Roe<std::vector<uint8_t>> RunLogBundle::Gzip(const std::string_view data) {
  z_stream stream{};
  // windowBits 15 + 16: a gzip header and trailer around the deflate stream.
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    return Error("gzip init failed");
  }
  std::vector<uint8_t> out(deflateBound(&stream, static_cast<uLong>(data.size())) + 32);
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  stream.avail_in = static_cast<uInt>(data.size());
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  const int result = deflate(&stream, Z_FINISH);
  const uLong written = stream.total_out;
  deflateEnd(&stream);
  if (result != Z_STREAM_END) {
    return Error("gzip failed");
  }
  out.resize(written);
  return out;
}

Roe<RunLogArchive> RunLogBundle::Build(const std::string& log_path, const LogRetention& retention,
                                       const std::uintmax_t max_bytes, const std::time_t now) {
  const std::vector<LogFileInfo> files = DirectoryFiles(log_path);
  if (files.empty()) {
    return Error("No log files");
  }
  // Raw bytes read per attempt: logs shrink about eight to ten times, so the first try usually fits a 4 MiB archive
  // while a phone holds at most a few copies of 16 MiB; the last raw budget is the limit itself, which gzip cannot grow
  // past it by more than its few bytes of framing.
  constexpr std::uintmax_t kMiB = 1024 * 1024;
  std::vector<std::uintmax_t> budgets = {16 * kMiB, 8 * kMiB};
  budgets.erase(std::remove_if(budgets.begin(), budgets.end(), [&](std::uintmax_t b) { return b <= max_bytes; }),
                budgets.end());
  budgets.push_back(max_bytes > 8192 ? max_bytes - 8192 : max_bytes / 2);

  const fs::file_time_type file_now = fs::file_time_type::clock::now();
  for (const std::uintmax_t budget : budgets) {
    const RunLogSelection selection = Select(files, file_now, retention, budget);
    std::string joined;
    for (const RunLogPick& pick : selection.files) {
      joined += "==== " + fs::path(pick.name).filename().string() + " ====\n";
      joined += FilterRecords(ReadTail(pick.name, pick.size, pick.take_bytes));
    }
    auto gz = Gzip(joined);
    if (!gz) {
      return gz.error();
    }
    if (gz->size() <= max_bytes) {
      RunLogArchive archive;
      archive.filename = "pp-run-logs-" + LocalDate(now) + ".log.gz";
      archive.mime = "application/gzip";
      archive.bytes = std::move(*gz);
      archive.first_time = FirstRecordTime(joined);
      archive.truncated = selection.truncated;
      return archive;
    }
  }
  return Error("Run logs do not fit the size limit");
}

} // namespace pbr
