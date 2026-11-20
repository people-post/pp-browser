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

bool IsSegmentEnd(const char c) {
  return c == '/' || c == '\\' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' || c == '\'';
}

/**
 * Where the folder name that starts at `from` ends: at the first separator (`/` or `\`), whitespace, quote or the end
 * of the record. A Windows user name may hold spaces: with `spaces_before_sep`, a name with spaces goes up to the
 * separator that follows within 64 bytes on the same line (no quote in between), else it stops at the first space.
 */
size_t SegmentEnd(const std::string& text, const size_t from, const bool spaces_before_sep) {
  size_t end = from;
  while (end < text.size() && !IsSegmentEnd(text[end])) {
    ++end;
  }
  if (!spaces_before_sep || end >= text.size() || (text[end] != ' ' && text[end] != '\t')) {
    return end;
  }
  constexpr size_t kMaxSegment = 64;
  for (size_t at = end; at < text.size() && at - from <= kMaxSegment; ++at) {
    if (text[at] == '\\' || text[at] == '/') {
      return at;
    }
    if (text[at] == '\n' || text[at] == '"' || text[at] == '\'') {
      break;
    }
  }
  return end;
}

/** `prefix<segment>` becomes `~` (what follows stays); the segment may be the last thing in the record. */
void ReplacePathSegment(std::string& text, const std::string_view prefix, const bool ignore_case,
                        const bool spaces_before_sep) {
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
    const size_t start = at + prefix.size();
    const size_t end = SegmentEnd(text, start, spaces_before_sep);
    if (end == start) {
      continue; // no segment
    }
    text.replace(at, end - at, "~");
  }
}

std::string ScrubPaths(std::string text) {
  // Longest first: the iOS container under /private, then the plain one.
  ReplacePathSegment(text, "/private/var/mobile/Containers/Data/Application/", false, false);
  ReplacePathSegment(text, "/var/mobile/Containers/Data/Application/", false, false);
  ReplacePathSegment(text, "/private/var/mobile/Containers/Shared/AppGroup/", false, false);
  ReplacePathSegment(text, "/var/mobile/Containers/Shared/AppGroup/", false, false);
  ReplacePathSegment(text, "/data/user/0/", false, false);
  ReplacePathSegment(text, "/data/data/", false, false);
  ReplacePathSegment(text, "/Users/", false, false);
  ReplacePathSegment(text, "/home/", false, false);
  for (const char drive : std::string_view("ABCDEFGHIJKLMNOPQRSTUVWXYZ")) {
    // Single backslashes, and doubled ones as in a JSON-escaped string.
    ReplacePathSegment(text, std::string(1, drive) + ":\\Users\\", true, true);
    ReplacePathSegment(text, std::string(1, drive) + ":\\\\Users\\\\", true, true);
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

/** One log file's filtered text, under its `==== name ====` line. */
struct Section {
  std::string header;
  std::string body;
  bool crash = false;
};

size_t RawBytes(const std::vector<Section>& sections) {
  size_t total = 0;
  for (const Section& section : sections) {
    total += section.header.size() + section.body.size();
  }
  return total;
}

std::string Join(const std::string& head, const std::vector<Section>& sections) {
  std::string joined;
  joined.reserve(head.size() + RawBytes(sections));
  joined += head;
  for (const Section& section : sections) {
    joined += section.header;
    joined += section.body;
  }
  return joined;
}

/** Drops about `drop` bytes from the front, oldest first: whole sections, then the head of the first left, up to the
 *  next record start. Returns false when nothing is left. */
bool DropOldest(std::vector<Section>& sections, size_t drop) {
  size_t at = 0;
  while (at < sections.size() && drop >= sections[at].header.size() + sections[at].body.size()) {
    drop -= sections[at].header.size() + sections[at].body.size();
    ++at;
  }
  sections.erase(sections.begin(), sections.begin() + static_cast<std::ptrdiff_t>(at));
  if (sections.empty()) {
    return false;
  }
  std::string& body = sections.front().body;
  size_t cut = std::max<size_t>(1, drop > sections.front().header.size() ? drop - sections.front().header.size() : 0);
  while (cut < body.size() &&
         !((cut == 0 || body[cut - 1] == '\n') && IsRecordHead(std::string_view(body).substr(cut)))) {
    const size_t nl = body.find('\n', cut);
    cut = nl == std::string::npos ? body.size() : nl + 1;
  }
  body.erase(0, cut);
  while (!sections.empty() && sections.front().body.empty()) {
    sections.erase(sections.begin());
  }
  return !sections.empty();
}

} // namespace

std::string RunLogBundle::ReadTail(const std::string& name, const std::uintmax_t size, const std::uintmax_t take) {
  std::ifstream in(name, std::ios::binary);
  if (!in) {
    return {};
  }
  const std::uintmax_t offset = take < size ? size - take : 0;
  // From the byte before the cut (when there is one): a cut that lands exactly on a line start keeps that line.
  const std::uintmax_t from = offset > 0 ? offset - 1 : 0;
  in.seekg(static_cast<std::streamoff>(from));
  std::string text(static_cast<size_t>(take + (offset > 0 ? 1 : 0)), '\0');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<size_t>(in.gcount()));
  const size_t last = text.rfind('\n');
  text.resize(last == std::string::npos ? 0 : last + 1); // a line still being written is left out
  if (offset > 0) {
    if (!text.empty() && text[0] == '\n') {
      text.erase(0, 1); // the cut fell on a line start: the first line is whole
    } else {
      const size_t first = text.find('\n');
      text.erase(0, first == std::string::npos ? text.size() : first + 1); // the cut line
    }
  }
  return text;
}

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

std::vector<CrashRecord> FileCrashRecordSource::Records() const {
  constexpr std::uintmax_t kCrashMax = 64 * 1024;
  std::error_code ec;
  if (path_.empty() || !fs::is_regular_file(path_, ec)) {
    return {};
  }
  std::ifstream in(path_, std::ios::binary);
  std::string text(kCrashMax, '\0');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<size_t>(in.gcount()));
  return {{fs::path(path_).filename().string(), std::move(text)}};
}

std::string RunLogBundle::FilterCrashRecord(const std::string_view text) {
  std::string out;
  size_t at = 0;
  while (at < text.size()) {
    const size_t end = text.find('\n', at);
    const std::string_view line = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
    std::string kept = FilterRecord(line);
    if (!kept.empty()) {
      out += kept;
      out += '\n';
    }
    if (end == std::string_view::npos) {
      break;
    }
    at = end + 1;
  }
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
                                       const std::uintmax_t max_bytes, const std::time_t now,
                                       const CrashRecordSource* crash, const std::string_view build) {
  const std::vector<LogFileInfo> files = DirectoryFiles(log_path);
  if (files.empty()) {
    return Error("No log files");
  }
  // Each file is read and filtered once, at most 16 MiB of raw text in all (a phone holds a few copies of that).
  constexpr std::uintmax_t kRawLimit = 16 * 1024 * 1024;
  const RunLogSelection selection = Select(files, fs::file_time_type::clock::now(), retention, kRawLimit);
  std::vector<Section> sections;
  for (const RunLogPick& pick : selection.files) {
    sections.push_back({"==== " + fs::path(pick.name).filename().string() + " ====\n",
                        FilterRecords(ReadTail(pick.name, pick.size, pick.take_bytes))});
  }
  if (crash != nullptr) {
    for (CrashRecord& record : crash->Records()) {
      std::string body = FilterCrashRecord(record.text);
      if (!body.empty()) {
        sections.push_back({"==== " + record.name + " (crash record) ====\n", std::move(body), true});
      }
    }
  }
  // The build heads the archive and is not a section: DropOldest never takes it.
  const std::string head = build.empty() ? std::string() : "==== build " + std::string(build) + " ====\n";
  bool truncated = selection.truncated;
  // Gzip, and while it is over `max_bytes` drop the oldest records (scaled by how far over it was, with a margin)
  // and gzip again; the newest records stay.
  constexpr int kMaxAttempts = 12;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    const std::string joined = Join(head, sections);
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
      archive.truncated = truncated;
      archive.crash_included = std::any_of(sections.begin(), sections.end(), [](const Section& s) { return s.crash; });
      return archive;
    }
    const double keep = 0.9 * static_cast<double>(max_bytes) / static_cast<double>(gz->size());
    const size_t raw = joined.size();
    const size_t drop = std::max<size_t>(1, raw - static_cast<size_t>(static_cast<double>(raw) * keep));
    truncated = true;
    if (!DropOldest(sections, drop)) {
      break;
    }
  }
  return Error("Run logs do not fit the size limit");
}

} // namespace pbr
