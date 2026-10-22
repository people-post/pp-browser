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

/**
 * V053: `sender_identity` may ask to join group call `call_id`: the call was offered to them (they
 * hold a participant row, in any state — declined, missed, left too) or they started it.
 */
bool SenderMayJoin(const CallSessionStore& sessions, const std::string& call_id, const std::string& sender_identity);

/** How long a group call this device dropped out of stays joinable without word that it ended. */
inline constexpr int64_t kJoinableGroupCallMs = 6LL * 60 * 60 * 1000;

/**
 * V053: the newest call of group `group_id` this device may join: offered to it, it is not in it
 * (declined, missed, left), someone else was last known to be in it, and it started within
 * kJoinableGroupCallMs. Word that the call ended marks everyone left, so it is no longer joinable.
 */
std::optional<CallSession> JoinableGroupCall(const CallSessionStore& sessions, const std::string& group_id,
                                             const std::string& local_identity, int64_t now_ms);

/**
 * Calling each other at once: a 1:1 call this device placed to `peer` (not `except_call_id`, not a
 * group's) that `peer` has not answered yet and nobody else is in.
 */
std::optional<CallSession> OurUnansweredCallTo(const CallSessionStore& sessions, const std::string& peer,
                                               const std::string& local_identity, const std::string& except_call_id);

/**
 * V053: two of a group calling it at once — a call of `group_id` this device placed (not
 * `except_call_id`) that nobody else has joined yet.
 */
std::optional<CallSession> OurUnansweredGroupCall(const CallSessionStore& sessions, const std::string& group_id,
                                                  const std::string& local_identity, const std::string& except_call_id);

/**
 * B30: a 1:1 call we are in whose one remote, `peer`, is still invited or ringing (its call-media
 * hello arriving first is it accepting).
 */
bool PeerStillToAnswerOurCall(const CallSessionStore& sessions, const std::string& call_id, const std::string& peer,
                              const std::string& local_identity);

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
