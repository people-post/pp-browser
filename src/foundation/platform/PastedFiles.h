#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace pbr {

/**
 * The files this app writes for a pasted picture (DEV/PLAN-desktop-drop-paste.md, D1): one folder, `<cache>/pasted`,
 * outside the account's profile. Only files inside that folder are ever deleted here; a user's own file never is.
 * The folder is always passed in (the app gives `AppPaths::CacheDir() / "pasted"`), so tests use their own.
 */

/** Writes `bytes` as "Pasted image <n>.<extension>" in `dir` (made if missing; a name not taken). The path, or nullopt: nothing is left behind then. Worker thread. */
std::optional<std::string> WritePastedFile(const std::filesystem::path& dir, const std::string& bytes,
                                           const std::string& extension);

/**
 * Deletes `path` when, after resolving `..` and links, it is a plain file inside `dir`; anything else (outside,
 * a link, a folder, `dir` itself) is left alone. True when a file was deleted.
 */
bool ReleasePastedFile(const std::string& path, const std::filesystem::path& dir);

/** Start-up: empties `dir` (its contents only; `dir` itself and anything else stay). Returns how many entries went. */
size_t SweepPastedDir(const std::filesystem::path& dir);

} // namespace pbr
