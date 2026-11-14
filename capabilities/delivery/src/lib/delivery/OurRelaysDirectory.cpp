#include "lib/delivery/OurRelaysDirectory.h"

#include "common/Utilities.h"
#include "lib/delivery/RelayOrigins.h"
#include "lib/people/DirectoryAnswers.h"
#include "lib/people/PeopleLookupFailure.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace pbr {

namespace {

/** One search's answers, filled from several threads; a late one lands here after the search has gone. */
struct PendingSearch {
  std::mutex mutex;
  std::condition_variable answered;
  std::vector<DirectoryAnswer> answers;
  std::vector<std::optional<Error>> errors;
  size_t waiting = 0;
};

/** Notes in `health` whether `origin` could be reached, by the error it gave (none: it answered). */
void NoteReachIn(RelayHealth& health, const std::string& origin, const Error* error) {
  if (!error || ClassifyPeopleLookupError(*error) == PeopleLookupFailure::NotFound) {
    health.NoteOk(origin);
  } else if (ClassifyPeopleLookupError(*error) == PeopleLookupFailure::DirectoryUnreachable) {
    health.NoteFailed(origin, util::NowUnixMs());
  }
}

/** Asks one directory and files its answer at `index`. */
void AskAndFile(const OurRelaysDirectory::Source& source, const std::string& query, size_t index,
                PendingSearch& pending) {
  auto hits = source.client->SearchPeople(query);
  std::lock_guard lock(pending.mutex);
  DirectoryAnswer& answer = pending.answers[index];
  if (hits) {
    answer.hits = std::move(*hits);
    answer.error.clear();
  } else {
    answer.error = hits.error().message;
    pending.errors[index] = hits.error();
  }
  --pending.waiting;
  pending.answered.notify_all();
}

std::shared_ptr<PendingSearch> NewPendingSearch(const std::vector<OurRelaysDirectory::Source>& sources) {
  auto pending = std::make_shared<PendingSearch>();
  for (const OurRelaysDirectory::Source& source : sources) {
    pending->answers.push_back(
        DirectoryAnswer{.origin = source.origin, .hits = std::nullopt, .error = "no answer in time"});
  }
  pending->errors.resize(sources.size());
  pending->waiting = sources.size();
  return pending;
}

} // namespace

OurRelaysDirectory::OurRelaysDirectory(std::vector<Source> sources, RunElsewhere run_elsewhere,
                                       const int64_t answer_timeout_ms)
    : sources_(std::move(sources)),
      run_elsewhere_(std::move(run_elsewhere)),
      answer_timeout_ms_(answer_timeout_ms),
      health_(std::make_shared<RelayHealth>()) {
  redirectLogger("OurRelaysDirectory");
}

void OurRelaysDirectory::NoteReach(const std::string& origin, const Error* error) {
  NoteReachIn(*health_, origin, error);
}

Roe<std::vector<mesh::DirectoryHit>> OurRelaysDirectory::SearchPeople(const std::string& query) {
  if (sources_.empty()) {
    return Error("Directory not configured");
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(answer_timeout_ms_);
  auto pending = NewPendingSearch(sources_);
  // The others first, so they run while the first is asked here. A task keeps what it needs, never this.
  std::vector<std::function<void()>> in_turn;
  for (size_t i = 1; i < sources_.size(); ++i) {
    auto ask = [pending, source = sources_[i], query, i, health = health_]() {
      AskAndFile(source, query, i, *pending);
      std::lock_guard lock(pending->mutex);
      NoteReachIn(*health, source.origin, pending->errors[i] ? &*pending->errors[i] : nullptr);
    };
    if (run_elsewhere_) {
      run_elsewhere_(std::move(ask));
    } else {
      in_turn.push_back(std::move(ask));
    }
  }
  AskAndFile(sources_[0], query, 0, *pending);
  for (const std::function<void()>& ask : in_turn) {
    ask();
  }
  std::vector<DirectoryAnswer> answers;
  std::optional<Error> first_error;
  {
    std::unique_lock lock(pending->mutex);
    pending->answered.wait_until(lock, deadline, [&pending] { return pending->waiting == 0; });
    answers = pending->answers;
    first_error = pending->errors[0];
  }
  NoteReach(sources_[0].origin, first_error ? &*first_error : nullptr);
  return Merged(answers, first_error);
}

Roe<std::vector<mesh::DirectoryHit>> OurRelaysDirectory::Merged(const std::vector<DirectoryAnswer>& answers,
                                                                const std::optional<Error>& first_error) {
  std::string unreached;
  bool any = false;
  for (const DirectoryAnswer& answer : answers) {
    if (answer.hits) {
      any = true;
    } else {
      unreached += (unreached.empty() ? "" : ", ") + RelayOriginKey(answer.origin) + " (" + answer.error + ")";
    }
  }
  if (!any) {
    return first_error ? *first_error : Error("Directory search: no answer in time");
  }
  if (!unreached.empty()) {
    log().warning << "people search without " << unreached; // the others' hits still count
  }
  return MergeDirectoryAnswers(answers);
}

std::vector<const OurRelaysDirectory::Source*> OurRelaysDirectory::LookupOrder() const {
  std::vector<std::string> origins;
  for (const Source& source : sources_) {
    origins.push_back(source.origin);
  }
  std::vector<const Source*> order;
  for (const std::string& origin : health_->Order(origins, util::NowUnixMs())) {
    for (const Source& source : sources_) {
      if (source.origin == origin) {
        order.push_back(&source);
        break;
      }
    }
  }
  return order;
}

Roe<mesh::DirectoryHit> OurRelaysDirectory::LookupByAccount(const std::string& account_id) {
  bool not_found = false;
  std::optional<Error> first_failure;
  for (const Source* source : LookupOrder()) {
    auto hit = source->client->LookupByAccount(account_id);
    NoteReach(source->origin, hit ? nullptr : &hit.error());
    if (hit) {
      return hit;
    }
    if (ClassifyPeopleLookupError(hit.error()) == PeopleLookupFailure::NotFound) {
      not_found = true; // that relay answered: the account is not registered there
    } else if (!first_failure) {
      first_failure = hit.error();
    }
  }
  if (not_found) {
    return Error("Account not found"); // where we could ask; a relay that failed does not fail the lookup
  }
  return first_failure ? *first_failure : Error("Directory not configured");
}

Roe<std::vector<mesh::MeshNodeHit>> OurRelaysDirectory::ListMeshNodes() {
  if (sources_.empty()) {
    return Error("Directory not configured");
  }
  return sources_.front().client->ListMeshNodes();
}

} // namespace pbr
