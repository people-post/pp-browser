#include "domain/people/ModelFacingJson.h"

#include "common/chat/MessagingJson.h"
#include "domain/people/ContactJson.h"

namespace pbr {

Object ContactForModel(const Contact& contact) {
  Object local;
  local.set("display_name", contact.local.display_name);
  local.set("trust", TrustLevelToString(contact.local.trust));
  Object remote;
  remote.set("nickname", contact.remote.nickname);
  Object out;
  out.set("id", contact.id);
  out.set("local", local);
  out.set("remote", remote);
  return out;
}

Object ThreadForModel(const Thread& thread) {
  Object json = ThreadToJson(thread);
  json.erase("preview");             // message content
  json.erase("peer_identity_value"); // the peer's account / key identifier
  return json;
}

} // namespace pbr
