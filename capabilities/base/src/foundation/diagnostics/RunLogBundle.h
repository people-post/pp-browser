#pragma once

#include "foundation/diagnostics/LogFile.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
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

/** The archive "Send run logs" sends, in memory only (never written to disk by this code). */
struct RunLogArchive {
  std::string filename; ///< `pp-run-logs-<YYYY-MM-DD>.log.gz`
  std::string mime;     ///< `application/gzip`
  std::vector<uint8_t> bytes;
  /** The first record's local time, `MM-DD HH:MM` ("from … to now"); empty when no record survived. */
  std::string first_time;
  bool truncated = false; ///< older records did not fit
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

  /** RFC 1952 gzip of `data`. */
  static Roe<std::vector<uint8_t>> Gzip(std::string_view data);

  /**
   * Reads the files Select picks from `log_path`'s directory (each up to its last complete line, a cut tail from its
   * first complete line), filters them, joins them oldest first under `==== <file name> ====` lines and gzips the
   * result; while the archive is over `max_bytes` the raw budget halves and it is built again (the newest records
   * stay). `now` names the file (local date).
   */
  static Roe<RunLogArchive> Build(const std::string& log_path, const LogRetention& retention, std::uintmax_t max_bytes,
                                  std::time_t now);
};

} // namespace pbr
