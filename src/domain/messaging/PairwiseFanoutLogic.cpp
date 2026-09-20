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

std::vector<std::string> SelectCallFanoutIdentities(const std::vector<CallParticipant>& participants,
                                                   const std::string& skip_identity, bool include_joined,
                                                   bool include_ringing_or_invited) {
  std::vector<std::string> out;
  out.reserve(participants.size());
  for (const CallParticipant& row : participants) {
    if (row.identity.empty() || row.identity == skip_identity) {
      continue;
    }
    const bool ok_joined = include_joined && row.state == CallParticipantState::Joined;
    const bool ok_ring =
        include_ringing_or_invited &&
        (row.state == CallParticipantState::Ringing || row.state == CallParticipantState::Invited);
    if (!ok_joined && !ok_ring) {
      continue;
    }
    out.push_back(row.identity);
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
