#pragma once

#include "app/node/NodeConfig.h"
#include "common/Error.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * pp-node's own config loader (capability-repos CR028): the file `--config` / PP_BROWSER_CONFIG names (none: the
 * defaults; never the desktop app's file), read for the node's keys only. It settles the backend as the app does (the
 * file's `backend.env`, a custom backend's services, the build's compiled backends; latched process-wide) and keeps the
 * app's rules: defaults merged with the file key by key, a built-in backend's registration over the file's, a newer
 * `config_version` refused. node_config_test holds it to the app's loader.
 */
Roe<NodeConfig> LoadNodeConfig(int argc, char** argv);

/** The file LoadNodeConfig reads: `--config <path>`, else PP_BROWSER_CONFIG; empty when none is named. */
std::string NodeConfigPath(int argc, char** argv);

/**
 * The warning for a file whose `registration` the running backend replaced: on a built-in backend the backend's own
 * registration runs, and the file's is ignored (a custom backend, `backend.env: custom`, takes the file's). None when
 * the file names none, or names what runs. `file` is the file's parsed root; `backend` the backend that runs.
 */
std::optional<std::string> IgnoredRegistrationNote(const Object& file, const NodeConfig& loaded,
                                                   const std::string& backend);

} // namespace pbr
