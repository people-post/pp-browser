#pragma once

#include <cstddef>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace pbr {

/**
 * The files this app writes for a pasted picture (DEV/PLAN-desktop-drop-paste.md, D1): one folder per run,
 * `<cache>/pasted/<run id>`, outside the account's profile. Only files inside that folder are ever deleted here; a user's own file never is.
 * The folders are always passed in (the app gives this run's), so tests use their own.
 */

/** Writes `bytes` as "Pasted image <n>.<extension>" in `dir` (made if missing; <n> counts up for the whole run, a name is never reused). The path, or nullopt: nothing is left behind then. Worker thread. */
std::optional<std::string> WritePastedFile(const std::filesystem::path& dir, const std::string& bytes,
                                           const std::string& extension);

/**
 * Deletes `path` when, after resolving `..` and links, it is a plain file inside `dir`; anything else (outside,
 * a link, a folder, `dir` itself) is left alone. True when a file was deleted.
 */
bool ReleasePastedFile(const std::string& path, const std::filesystem::path& dir);

/** A fresh random id for this run's folder (hex). */
std::string NewPastedRunId();

/**
 * Start-up: removes the OTHER runs' folders under `root` (`<cache>/pasted`) that are stale: nothing in them written in
 * the last `stale_after` (a run that is still going writes there; a crashed or killed one does not). `own_run_dir` and
 * anything outside `root` are never touched; a link is removed as itself. Returns how many entries went. Another
 * running instance (even of another profile) shares the cache folder, so a sweep may never empty `root` itself.
 * Chosen over a lock file: no platform code, and a run idle for a day has no preview page worth keeping.
 */
size_t SweepStalePastedRuns(const std::filesystem::path& root, const std::filesystem::path& own_run_dir,
                            std::chrono::hours stale_after = std::chrono::hours(24),
                            std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now());

} // namespace pbr
