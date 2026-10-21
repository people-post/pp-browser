#pragma once

#include "domain/calls/CallSessionStore.h"
#include "domain/calls/CallTypes.h"

#include "common/Error.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What a call's stored session and participants say: the reads the session manager, the topology and
 * the workflow share. Over the store only; no side effects.
 */
namespace call_queries {

/** The call's roster as a CallRoster carries it (participants, their media, the media epoch). */
Roe<CallRosterDetail> RosterDetail(const CallSessionStore& sessions, const std::string& call_id);

/** The first participant that is not `local_identity` (a 1:1 call's peer); none if there is none. */
Roe<std::optional<CallParticipant>> FirstRemoteParticipant(const CallSessionStore& sessions,
                                                           const std::string& call_id,
                                                           const std::string& local_identity);

/** Participants in state Joined. */
Roe<std::vector<CallParticipant>> JoinedParticipants(const CallSessionStore& sessions, const std::string& call_id);

/** Identities of joined participants other than `local_identity` (empty on a store error). */
std::vector<std::string> JoinedRemoteIdentities(const CallSessionStore& sessions, const std::string& call_id,
                                                const std::string& local_identity);

/** A participant whose invite was accepted implicitly: the call waits for their explicit answer. */
Roe<bool> AwaitingExplicitAnswer(const CallSessionStore& sessions, const std::string& call_id);

/** A joined remote that `local_identity` invited has not accepted yet (its invite still pending). */
bool AwaitsOwnInviteeAccept(const CallSessionStore& sessions, const std::string& call_id,
                            const std::string& local_identity);

/** `local_identity` is the call's sticky initiator (V021): the earliest joiner (SelectCallInitiator). */
bool IsStickyInitiator(const CallSessionStore& sessions, const std::string& call_id,
                       const std::string& local_identity);

/**
 * `sender_identity` is in call `call_id`: its initiator, or a participant invited, ringing or joined.
 * Controls other than an invite or a summary are taken only from them.
 */
bool SenderIsInCall(const CallSessionStore& sessions, const std::string& call_id,
                    const std::string& sender_identity);

} // namespace call_queries

/**
 * A peer's CallRoster applied to the stored participants. The sender's view of us is stale and only we
 * know our own media: of our own entry only the join stamp is taken (V050 stamps joins on the inviter's
 * clock). A stale fan-out does not resurrect a peer who left, declined or missed, nor demote a joined
 * one to ringing or invited (its media still updates). The media epoch follows the roster.
 */
Roe<void> ApplyCallRosterToStore(const CallSessionStore& sessions, const CallRosterDetail& roster,
                                 const std::string& local_identity);

} // namespace pbr
