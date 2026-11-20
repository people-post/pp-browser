#pragma once

#include <map>
#include <mutex>
#include <string>

namespace pbr {

/**
 * Remote switches (release-compatibility O2; SERVICE_ENDPOINTS § Client compatibility): the org relay's
 * `client-compat` `switches`, read once a run, from the first document the app's start-up check resolves (the cached
 * copy when it is fresh, else the relay's answer); a later fetch is cached and takes effect at the next start, so a
 * switch never flips under a running feature. A switch the document does not name is its caller's built-in default;
 * one the app does not know is never asked for. No product switch is defined in release 1: a feature that wants one
 * names it (a constant beside its code, `a-z0-9_.-`) and asks IsOn(name, its default). Thread-safe.
 */
class RemoteSwitches {
public:
  /** The run's switches (ClientCompatController, at start-up). The first call wins; later ones are ignored. */
  static void Load(const std::map<std::string, bool>& switches);
  /** Whether `name` is on: the document's value when it names it, else `built_in_default`. */
  static bool IsOn(const std::string& name, bool built_in_default);
  /** The run's switches were loaded (a document was resolved at start-up). */
  static bool Loaded();
  /** Tests: forget the run's switches. */
  static void ResetForTest();

private:
  static std::mutex& Mutex();
  static std::map<std::string, bool>& Switches();
  static bool& LoadedFlag();
};

} // namespace pbr
