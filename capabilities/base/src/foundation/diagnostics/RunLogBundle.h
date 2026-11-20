#pragma once

#include "foundation/diagnostics/LogFile.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbr {

/** One log file chosen for the archive: its last `take_bytes` (its whole size when it fits). */
struct RunLogPick {
  std::string name;
  std::uintmax_t size = 0;
  std::uintmax_t take_bytes = 0;
};

struct RunLogSelection {
  std::vector<RunLogPick> files; ///< oldest first
  bool truncated = false;        ///< a file was left out or cut to its tail
};

/** One crash record offered for the archive: a name for its header line and its raw text. */
struct CrashRecord {
  std::string name;
  std::string text;
};

/**
 * Where the crash records for the archive come from ("crash records available for the archive"). The app's own
 * pending record is one source (FileCrashRecordSource); a system-provided one can be added behind the same interface.
 */
class CrashRecordSource {
public:
  virtual ~CrashRecordSource() = default;
  /** Newest first; each text at most 64 KiB. Called off the UI thread. */
  virtual std::vector<CrashRecord> Records() const = 0;
};

/** The app's own record: CrashDump's `crash_pending.txt` (one file; none when it does not exist). */
class FileCrashRecordSource final : public CrashRecordSource {
public:
  explicit FileCrashRecordSource(std::string path) : path_(std::move(path)) {}
  std::vector<CrashRecord> Records() const override;

private:
  std::string path_;
};

/** The archive "Send run logs" sends, in memory only (never written to disk by this code). */
struct RunLogArchive {
  std::string filename; ///< `pp-run-logs-<YYYY-MM-DD>.log.gz`
  std::string mime;     ///< `application/gzip`
  std::vector<uint8_t> bytes;
  /** The first record's local time, `MM-DD HH:MM` ("from … to now"); empty when no record survived. */
  std::string first_time;
  bool truncated = false;      ///< older records did not fit
  bool crash_included = false; ///< at least one crash record is in the archive
};

/**
 * "Send run logs" (DEV/PLAN-send-run-logs.md): the log directory's files, chosen by the same retention that prunes
 * them, filtered record by record and gzipped under a size limit. Everything here is pure except Build, which reads
 * the files (call it off the UI thread).
 */
class RunLogBundle {
public:
  /** Every file of the three families next to `log_path` (pp-browser, metrics, diag), as LogFile::FamilyFiles. */
  static std::vector<LogFileInfo> DirectoryFiles(const std::string& log_path);

  /**
   * Whether "Send run logs" can be tapped: always with `collect` on; otherwise only when `main_family` (the
   * pp-browser files, LogFile::FamilyFiles(log_path)) holds a file besides this launch's.
   */
  static bool Sendable(const std::vector<LogFileInfo>& main_family, bool collect);

  /**
   * Pure. Within each family (`<stem>` / `<stem>.<N>`) only the files `retention` keeps (LogFile::ExpiredFiles: off
   * keeps this launch and the one before, whatever else is on disk); then newest first while they fit in
   * `size_limit`, the first that does not fit cut to its tail; the result oldest first.
   */
  static RunLogSelection Select(std::vector<LogFileInfo> files, std::filesystem::file_time_type now,
                                const LogRetention& retention, std::uintmax_t size_limit);

  /**
   * Pure. Log text with what could be a person's words taken out, record by record: a record starts with the
   * logger's `[YYYY-MM-DD hh:mm:ss` and runs over the lines after it (lines before the first record are dropped). A
   * record with any of text= body= content= query= question= prompt= draft= caption= raw= goes whole; the values of
   * name= display_name= nickname= title= filename= path= subtitle= user= become `<hidden>` up to the next ` key=`
   * (a quoted value: to its closing quote); home and app-container paths become `~`. A key counts only at the start
   * or after a space, tab, `,`, `;`, `(`, `[`, `{`, `?`, `&`, quote, `:` or `/`, never after a letter, digit or `_` (so
   * hostname= is not name=).
   */
  static std::string FilterRecords(std::string_view text);

  /**
   * Pure. The pending crash record (CrashDump: signal or reason, version, frames, the last log lines) with the same
   * filter as the logs, line by line (the record's lines are not log records): a line with a drop key goes, mask keys
   * and home paths are scrubbed. Empty when nothing is left.
   */
  static std::string FilterCrashRecord(std::string_view text);

  /** RFC 1952 gzip of `data`. */
  static Roe<std::vector<uint8_t>> Gzip(std::string_view data);

  /**
   * The last `take` bytes of `name` (a file of `size` bytes) from its first to its last complete line: a line still
   * being written is left out, and a cut that falls inside a line drops that line (one that falls exactly on a line
   * start keeps it).
   */
  static std::string ReadTail(const std::string& name, std::uintmax_t size, std::uintmax_t take);

  /**
   * Reads each file Select picks from `log_path`'s directory once (at most 16 MiB of raw text in all; ReadTail per
   * file) and filters it once, joins them oldest first under `==== <file name> ====` lines and gzips the result.
   * While the archive is over `max_bytes`, the oldest records are dropped (whole files first, then the head of the
   * next, at a record start), by about how far over it was plus a 10% margin, and it is gzipped again, up to 12
   * times; the newest records stay. A non-null `crash` (only when "Collect crash logs" is on) adds its records
   * (read once) after the logs, each under its own header, filtered by FilterCrashRecord and counted in the size;
   * they are the last to be dropped. A non-empty `build` (the app's version, tag and commit) heads the archive as
   * `==== build <build> ====` and is never dropped, so the archive names the build that sent it whatever the logs
   * kept. Fails when nothing fits. `now` names the file (local date).
   */
  static Roe<RunLogArchive> Build(const std::string& log_path, const LogRetention& retention, std::uintmax_t max_bytes,
                                  std::time_t now, const CrashRecordSource* crash = nullptr,
                                  std::string_view build = {});
};

} // namespace pbr
