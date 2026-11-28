#pragma once

#include "app/node/NodeConfig.h"
#include "common/Error.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

struct AppConfig;

/** The node's part of `config` (CR028): the one place pp-node meets the app's config type. */
NodeConfig NodeConfigFrom(const AppConfig& config);

/**
 * Loads the file `--config` / PP_BROWSER_CONFIG names (none: defaults; never the desktop app's file) with the app's
 * loader, so its defaults and the backend it settles are the app's, and keeps the node's part.
 */
Roe<NodeConfig> LoadNodeConfig(int argc, char** argv);

/**
 * The warning for a file whose `registration` the running backend replaced: on a built-in backend the backend's own
 * registration runs, and the file's is ignored (a custom backend, `backend.env: custom`, takes the file's). None when
 * the file names none, or names what runs. `file` is the file's parsed root.
 */
std::optional<std::string> IgnoredRegistrationNote(const Object& file, const AppConfig& loaded);

/** The file LoadNodeConfig reads; empty when none is named. */
std::string NodeConfigPath(int argc, char** argv);

} // namespace pbr
