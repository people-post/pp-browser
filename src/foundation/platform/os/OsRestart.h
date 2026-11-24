#pragma once

#include <string>
#include <vector>

namespace pbr::os {

/**
 * Starting the same program again, with the same arguments and the same environment (the owner runs two instances with
 * different PP_BROWSER_CONFIG / HOME / --log-file / --debug: a restart must come back on the same profile). Desktop only;
 * iOS and Android answer RestartSupported() == false and the in-process path applies there.
 */
struct RestartPlan {
  std::string exe;               // the program file to start; empty: no plan could be made
  std::vector<std::string> argv; // argv[0] and the arguments exactly as the process got them
  bool Ready() const { return !exe.empty() && !argv.empty(); }
};

/** main() hands over its argc / argv first thing; a copy is kept. Later calls are ignored. */
void CaptureStartArguments(int argc, char** argv);
const std::vector<std::string>& StartArguments();

/** The plan for the arguments captured at start and the running program's file; not Ready() without either. */
RestartPlan PlanRestart(const std::string& exe_path, const std::vector<std::string>& start_args);

/** This build can start itself again (a desktop OS with an implementation). */
bool RestartSupported();

/**
 * Starts the plan's program, the caller then ends this process at once. Linux replaces the process (execv: returns only
 * on failure); macOS and Windows start a new process (true: started, the caller exits). The environment is inherited
 * as it is: nothing is added or removed. false with `error` when nothing was started.
 */
bool RestartProcess(const RestartPlan& plan, std::string& error);

} // namespace pbr::os
