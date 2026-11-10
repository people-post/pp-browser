#pragma once

#include <string>

namespace pbr {

/**
 * What an action did, as a kind and the name it concerns: the app words it in the user's language (the
 * capability never reads the locale). `name` is a chat title, a group title or a contact's name.
 */
struct ActionNotice {
  enum class Kind {
    OpenedConversation,
    OpenedSecureConversation,
    AddedContact,
    OpenedThread,
    Registered,
    RegistrationFailed,
    JoinedGroup,
    DeclinedInvitation,
    BlockedInviter,
    BlockedUnknownInviter,
    CreatedGroup,
    StartedFresh,
    OpenedOwnerChat,
  };
  Kind kind = Kind::OpenedThread;
  std::string name;
};

} // namespace pbr
