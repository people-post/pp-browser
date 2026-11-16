#pragma once

#include "lib/delivery/RelayHealth.h"
#include "lib/people/DirectoryAnswers.h"
#include "common/Module.h"
#include "mesh/types/IDirectoryClient.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * The directory as our relays answer it (multi-relay R2; DESIGN "A name search asks the relays the user has listed"):
 * the configured directory first (normally the preferred relay's, with its failover providers), then the directory of
 * each other relay of ours.
 *
 * - **A name search** asks all of them at once — the first on the caller's thread, the others elsewhere (a worker) —
 *   waits for the others until kAnswerTimeoutMs from its start, and merges what came (MergeDirectoryAnswers: the
 *   first one's order first, each account once, its entry the one whose keys verify, then the newer). A directory
 *   that fails or does not answer in time adds nothing and fails nothing: the search fails only when none answered,
 *   with the first one's error.
 * - **A lookup by Account ID** asks them in order — one that could not be reached lately after the others
 *   (RelayHealth) — and stops at the first that knows the account. Not found anywhere it reached: not found; reached
 *   none: the first failure.
 * - **Mesh nodes** are the first one's, as before.
 *
 * Thread-safe; a search's late answer is dropped.
 */
class OurRelaysDirectory : public Module, public mesh::IDirectoryClient {
public:
  /** How long a search waits for the directories after the first, from its start. */
  static constexpr int64_t kAnswerTimeoutMs = 10 * 1000;

  struct Source {
    /** Its base URL (for the log and the health memory). */
    std::string origin;
    std::shared_ptr<mesh::IDirectoryClient> client;
  };
  /** Runs `task` on another thread (a worker); none: a search asks each directory in turn on the caller's. */
  using RunElsewhere = std::function<void(std::function<void()> task)>;

  OurRelaysDirectory(std::vector<Source> sources, RunElsewhere run_elsewhere,
                     int64_t answer_timeout_ms = kAnswerTimeoutMs);

  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& account_id) override;
  Roe<std::vector<mesh::MeshNodeHit>> ListMeshNodes() override;

private:
  /** Notes whether `origin` could be reached, by the error it gave (none: it answered). */
  void NoteReach(const std::string& origin, const Error* error);
  /** One list from what came; none came: the first one's error. Logs which gave none. */
  Roe<std::vector<mesh::DirectoryHit>> Merged(const std::vector<DirectoryAnswer>& answers,
                                              const std::optional<Error>& first_error);
  std::vector<const Source*> LookupOrder() const;

  std::vector<Source> sources_;
  RunElsewhere run_elsewhere_;
  int64_t answer_timeout_ms_;
  /** Shared with a search's tasks, which may finish after this is gone. */
  std::shared_ptr<RelayHealth> health_;
};

} // namespace pbr
