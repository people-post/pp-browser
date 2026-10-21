#include "domain/messaging/PairwiseFanoutLogic.h"

namespace pbr {

std::vector<std::string> SelectPairwiseFanoutTargets(
    const std::vector<std::string>& identities, const std::string& skip_identity,
    const std::function<bool(const std::string& identity)>& include) {
  std::vector<std::string> out;
  out.reserve(identities.size());
  for (const std::string& id : identities) {
    if (id.empty() || id == skip_identity) {
      continue;
    }
    if (include && !include(id)) {
      continue;
    }
    out.push_back(id);
  }
  return out;
}


PairwiseFanoutResult FanOutPairwise(
    const std::vector<std::string>& targets, PairwiseFanoutMode mode,
    const std::function<pp::Roe<void>(const std::string& identity)>& send) {
  PairwiseFanoutResult result;
  if (!send) {
    result.first_error = pp::Error("Pairwise fan-out send not bound");
    return result;
  }
  for (const std::string& identity : targets) {
    ++result.attempted;
    auto sent = send(identity);
    if (sent) {
      ++result.succeeded;
      continue;
    }
    result.failed_identities.push_back(identity);
    result.failure_messages.push_back(sent.error().message);
    if (mode == PairwiseFanoutMode::FailFast) {
      result.first_error = sent.error();
      break;
    }
  }
  return result;
}

} // namespace pbr
