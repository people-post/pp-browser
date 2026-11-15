// Cross-implementation vectors for every signed relay command (multi-relay MR007; docs/contracts/SERVICE_ENDPOINTS.md
// § Signed relay commands): each domain and relay API op signed by one account for relay `a`, with what relays `a` and
// `b` must answer, the PeerIds of a fixed relay key, an account card of version 4 and cards that must be refused. The
// relay (pp-relay-apis `relay-sign-vectors.json`) copies the committed file byte for byte; a change to either side's
// bytes changes it here first. The file is checked here; PP_WRITE_RELAY_SIGN_VECTORS=<path> writes a fresh one (keys
// from fixed seeds; ML-DSA signatures are randomized, so only they differ between runs). Delivery sits above net and
// people, whose builders it needs.

#include "lib/delivery/RelayMessageSignPayload.h"

#include "common/ValueJson.h"
#include "common/chat/MessagingJson.h"
#include "common/chat/RelayStreamKey.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/Sha256Hex.h"
#include "lib/net/HttpLockerClient.h"
#include "lib/net/RegistrationSignPayload.h"
#include "lib/net/RelayApiSignPayload.h"
#include "lib/net/RelayBlobSignPayload.h"
#include "lib/people/AccountCard.h"
#include "lib/people/AccountLocker.h"

#include "crypto/PeerId.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

using ::pp::kMlDsa65SeedBytes;

constexpr int64_t kNowMs = 1'761'235'200'000;
constexpr char kFormat[] = "pp-browser relay sign vectors v1";
constexpr char kOriginA[] = "https://relay-a.example/api/relay";
constexpr char kOriginB[] = "https://relay-b.example/api/relay";
constexpr char kBlobId[] = "00000000-0000-4000-8000-000000000001";

/** Every key of the vectors, from fixed seeds. */
struct VectorKeys {
  MlDsaKeyPair relay_a;
  MlDsaKeyPair relay_b;
  std::string peer_a;
  std::string peer_b;
  MlDsaKeyPair account;
  HybridKemKeyPair kem;
  std::string account_id;
  MlDsaKeyPair device;
  HybridKemKeyPair device_kem;
  std::string device_peer_id;
};

MlDsaKeyPair KeysOfSeed(uint8_t fill) {
  return MlDsa::GenerateKeyPairFromSeed(ByteVector(kMlDsa65SeedBytes, fill)).value();
}

VectorKeys MakeKeys() {
  VectorKeys keys{.relay_a = KeysOfSeed(0x07), .relay_b = KeysOfSeed(0x08), .account = KeysOfSeed(0x09)};
  keys.peer_a = pp::PeerIdFromMlDsaPublicKey(keys.relay_a.public_key).value();
  keys.peer_b = pp::PeerIdFromMlDsaPublicKey(keys.relay_b.public_key).value();
  keys.kem = HybridKem::GenerateKeyPairFromSeed(ByteVector(64, 0x0a)).value();
  keys.account_id = AccountIdFromMlDsaPublicKey(keys.account.public_key).value();
  keys.device = KeysOfSeed(0x0b);
  keys.device_kem = HybridKem::GenerateKeyPairFromSeed(ByteVector(64, 0x0c)).value();
  keys.device_peer_id = pp::PeerIdFromMlDsaPublicKey(keys.device.public_key).value();
  return keys;
}

std::string Sign(const VectorKeys& keys, const ByteVector& bytes) {
  return Base64Encode(MlDsa::Sign(keys.account.secret_key, bytes).value());
}

std::string Str(const Object& body, const char* key) {
  return body.getString(key).value_or("");
}

uint64_t Num(const Object& body, const char* key) {
  return body.getNonNegInt(key).value_or(0);
}

int64_t Timestamp(const Object& body) {
  return body.getIf<int64_t>("timestamp").value_or(0);
}

/**
 * One signed command: where it goes, the fields of its body besides `audience`, `timestamp` and `signature`, and how
 * its sign bytes come from a body for an audience (as the relay's parser builds them).
 */
struct CallSpec {
  std::string name;
  std::string domain;
  int64_t version = 0;
  int64_t op = -1;
  std::string path;
  Object fields;
  std::function<ByteVector(const Object& body, const std::string& audience)> bytes;
};

/** `account_id` and one more string field, then the timestamp: the storage commands' commonest layout. */
CallSpec AccountTextCall(const std::string& name, const std::string& domain, const std::string& path,
                         const std::string& account_id, const char* key, const std::string& value,
                         std::vector<uint8_t> (*build)(const std::string&, const std::string&, const std::string&,
                                                       int64_t)) {
  Object fields;
  fields.set("account_id", account_id);
  fields.set(key, value);
  return CallSpec{.name = name,
                  .domain = domain,
                  .version = 2,
                  .path = path,
                  .fields = std::move(fields),
                  .bytes = [build, key](const Object& body, const std::string& audience) {
                    return build(audience, Str(body, "account_id"), Str(body, key), Timestamp(body));
                  }};
}

std::vector<CallSpec> RegistryCalls(const VectorKeys& keys) {
  const std::string pk = Base64Encode(keys.account.public_key);
  const std::string kem = Base64Encode(keys.kem.public_key);
  Object finish;
  finish.set("challenge", std::string("golden-challenge"));
  finish.set("public_key", pk);
  finish.set("nickname", std::string("alice"));
  finish.set("signature_alg", std::string("ml-dsa-65"));
  finish.set("kem_public_key_b64", kem);
  finish.set("kem_binding_signature_b64", Sign(keys, KemBindingSignBytes(pk, kem)));
  Object revoke;
  revoke.set("account_id", keys.account_id);
  revoke.set("registration_id", std::string("golden_registration_01"));
  Object nickname;
  nickname.set("account_id", keys.account_id);
  nickname.set("nickname", std::string("alice"));
  Object heartbeat;
  heartbeat.set("account_id", keys.account_id);
  heartbeat.set("peer_id", keys.device_peer_id);
  heartbeat.set("state", std::string("active"));
  Object llm;
  llm.set("account_id", keys.account_id);
  return {
      {.name = "register.finish",
       .domain = "pp-browser:relay-register-v1",
       .version = 3,
       .path = "/v1/register/finish",
       .fields = std::move(finish),
       .bytes =
           [](const Object& body, const std::string& audience) {
             return BuildRegistrationSignBytes(audience, Str(body, "challenge"), Str(body, "public_key"),
                                               Str(body, "kem_public_key_b64"), Str(body, "signature_alg"),
                                               Timestamp(body));
           }},
      {.name = "register.revoke",
       .domain = "pp-browser:relay-revoke-v1",
       .version = 2,
       .path = "/v1/register/revoke",
       .fields = std::move(revoke),
       .bytes =
           [](const Object& body, const std::string& audience) {
             return BuildAccountRevokeSignBytes(audience, Str(body, "account_id"), Str(body, "registration_id"),
                                                Timestamp(body));
           }},
      {.name = "profile.nickname",
       .domain = "pp-browser:relay-profile-v1",
       .version = 2,
       .path = "/v1/profile/nickname",
       .fields = std::move(nickname),
       .bytes =
           [](const Object& body, const std::string& audience) {
             return BuildProfileUpdateSignBytes(audience, Str(body, "account_id"), Str(body, "nickname"),
                                                Timestamp(body));
           }},
      {.name = "mesh.heartbeat",
       .domain = "pp-browser:relay-heartbeat-v1",
       .version = 2,
       .path = "/v1/mesh/heartbeat",
       .fields = std::move(heartbeat),
       .bytes =
           [](const Object& body, const std::string& audience) {
             return BuildRelayHeartbeatSignBytes(audience, Str(body, "account_id"), Str(body, "peer_id"),
                                                 Str(body, "state"), Timestamp(body));
           }},
      {.name = "llm.token.renew",
       .domain = "pp-browser:relay-llm-token-v1",
       .version = 2,
       .path = "/api/llm/v1/token/renew",
       .fields = std::move(llm),
       .bytes =
           [](const Object& body, const std::string& audience) {
             return BuildLlmTokenRenewSignBytes(audience, Str(body, "account_id"), Timestamp(body));
           }},
  };
}

CallSpec LockerCall(const VectorKeys& keys) {
  const ByteVector locker_id(32, 0xab);
  const ByteVector sealed(48, 0x5e);
  LockerRecord record{.locker_id = BytesToHex(locker_id),
                      .version = 3,
                      .signing_public_key_b64 = Base64Encode(keys.account.public_key),
                      .sealed_b64 = Base64Encode(sealed)};
  record.signature_b64 = Sign(keys, account_locker::SignBytes(locker_id, record.version, sealed)); // the owner half
  Object fields;
  fields.set("account_id", keys.account_id);
  fields.set("locker", ObjectValue(LockerRecordToJson(record)));
  return {.name = "lockers.put",
          .domain = "pp-browser:relay-locker-put-v1",
          .version = 2,
          .path = "/v1/lockers/put",
          .fields = std::move(fields),
          .bytes = [](const Object& body, const std::string& audience) {
            const Object* locker = body.getObject("locker");
            return locker ? BuildLockerPutSignBytes(audience, Str(body, "account_id"), Str(*locker, "locker_id"),
                                                    Num(*locker, "version"), Timestamp(body))
                          : ByteVector{};
          }};
}

std::vector<CallSpec> StorageCalls(const VectorKeys& keys) {
  const std::string& account = keys.account_id;
  Object presign;
  presign.set("account_id", account);
  presign.set("content_type", std::string("image/jpeg"));
  presign.setJsonUInt("byte_length", 2048);
  presign.set("purpose", std::string("icon"));
  Object icon;
  icon.set("account_id", account);
  icon.set("url", std::string(""));
  icon.set("blob_id", std::string(kBlobId));
  icon.set("kind", std::string("image/jpeg"));
  Object start;
  start.set("account_id", account);
  start.setJsonUInt("byte_length", 1048576);
  Object part;
  part.set("account_id", account);
  part.set("blob_id", std::string(kBlobId));
  part.setJsonUInt("part_number", 3);
  std::vector<CallSpec> calls{
      {.name = "blobs.presign",
       .domain = "pp-browser:relay-blob-presign-v1",
       .version = 2,
       .path = "/v1/blobs/presign",
       .fields = std::move(presign),
       .bytes =
           [](const Object& body, const std::string& audience) {
             return BuildBlobPresignSignBytes(audience, Str(body, "account_id"), Str(body, "content_type"),
                                              Num(body, "byte_length"), Str(body, "purpose"), Timestamp(body));
           }},
      {.name = "profile.icon",
       .domain = "pp-browser:relay-profile-icon-v1",
       .version = 2,
       .path = "/v1/profile/icon",
       .fields = std::move(icon),
       .bytes =
           [](const Object& body, const std::string& audience) {
             return BuildProfileIconSignBytes(audience, Str(body, "account_id"), Str(body, "url"), Str(body, "blob_id"),
                                              Str(body, "kind"), Timestamp(body));
           }},
      {.name = "files.start",
       .domain = "pp-browser:relay-file-start-v1",
       .version = 2,
       .path = "/v1/files/start",
       .fields = std::move(start),
       .bytes =
           [](const Object& body, const std::string& audience) {
             return BuildFileStartSignBytes(audience, Str(body, "account_id"), Num(body, "byte_length"),
                                            Timestamp(body));
           }},
      {.name = "files.part",
       .domain = "pp-browser:relay-file-part-v1",
       .version = 2,
       .path = "/v1/files/part",
       .fields = std::move(part),
       .bytes =
           [](const Object& body, const std::string& audience) {
             return BuildFilePartSignBytes(audience, Str(body, "account_id"), Str(body, "blob_id"),
                                           Num(body, "part_number"), Timestamp(body));
           }},
  };
  calls.push_back(AccountTextCall("blobs.retain", "pp-browser:relay-blob-retain-v1", "/v1/blobs/retain", account,
                                  "blob_id", kBlobId, BuildBlobRetainSignBytes));
  calls.push_back(AccountTextCall("blobs.delete", "pp-browser:relay-blob-delete-v1", "/v1/blobs/delete", account,
                                  "blob_id", kBlobId, BuildBlobDeleteSignBytes));
  calls.push_back(AccountTextCall("blobs.list", "pp-browser:relay-blob-list-v1", "/v1/blobs/list", account, "status",
                                  "retained", BuildBlobListSignBytes));
  calls.push_back(AccountTextCall("files.status", "pp-browser:relay-file-status-v1", "/v1/files/status", account,
                                  "blob_id", kBlobId, BuildFileStatusSignBytes));
  calls.push_back(AccountTextCall("files.complete", "pp-browser:relay-file-complete-v1", "/v1/files/complete", account,
                                  "blob_id", kBlobId, BuildFileCompleteSignBytes));
  calls.push_back(AccountTextCall("files.delete", "pp-browser:relay-file-delete-v1", "/v1/files/delete", account,
                                  "blob_id", kBlobId, BuildFileDeleteSignBytes));
  calls.push_back(AccountTextCall("files.list", "pp-browser:relay-file-list-v1", "/v1/files/list", account, "status",
                                  "pending", BuildFileListSignBytes));
  return calls;
}

/** A relay API call (`pp-browser:relay-api-v1` v3). */
CallSpec ApiCall(const std::string& name, RelayApiOp op, const std::string& path, Object fields,
                 std::function<ByteVector(const Object&, const std::string&)> bytes) {
  return CallSpec{.name = "relay-api." + name,
                  .domain = "pp-browser:relay-api-v1",
                  .version = 3,
                  .op = static_cast<int64_t>(op),
                  .path = path,
                  .fields = std::move(fields),
                  .bytes = std::move(bytes)};
}

ChatHistoryRequest HistoryRequest(const std::string& account_id) {
  ChatHistoryRequest request;
  request.requester_identity_kind = "account";
  request.requester_identity_value = account_id;
  request.peer_identity_kind = "account";
  request.peer_identity_value = "account:rXRl8_CuSzpM1FI15SM4hzrcy5NjJOp2fdmeA6mdmL0";
  request.channel = ThreadChannel::E2ePublic;
  request.session_epoch = 1;
  request.min_sender_seq = 4;
  request.limit = 50;
  request.order = "desc";
  return request;
}

std::vector<CallSpec> MessageCalls(const VectorKeys& keys) {
  const std::string& account = keys.account_id;
  RelayWireSendRecord send;
  send.sender_account_id = account;
  send.recipient_account_id = account; // a copy for our own other devices: the account is registered there
  send.stream_id = "v2:e2e_public:" + account + ":" + account + ":from:" + keys.device_peer_id;
  send.index_key = 7;
  send.blob_b64 = "AA==";
  Object send_fields = RelayWireSendRecordToJson(send);
  for (const char* signed_key : {"audience", "timestamp", "signature"}) {
    send_fields.erase(signed_key);
  }
  const auto reader = [account](const char* cursor_key, const std::string& cursor) {
    Object fields;
    fields.set("requester_account_id", account);
    fields.set(cursor_key, cursor);
    return fields;
  };
  const auto device = [&account](const char* last_key, const std::string& last) {
    Object fields;
    fields.set("account_id", account);
    fields.set("platform", std::string("android"));
    fields.set("device_id", std::string("golden-device"));
    fields.set(last_key, last);
    return fields;
  };
  const ChatHistoryRequest history = HistoryRequest(account);
  using B = std::function<ByteVector(const Object&, const std::string&)>;
  const B send_bytes = [](const Object& body, const std::string& audience) {
    RelayWireSendRecord record;
    record.sender_account_id = Str(body, "sender_account_id");
    record.recipient_account_id = Str(body, "recipient_account_id");
    record.stream_id = Str(body, "stream_id");
    record.index_key = Num(body, "index_key");
    return BuildRelayApiSendSignBytes(audience, record, Timestamp(body));
  };
  const auto reader_bytes = [](auto build, const char* cursor_key) {
    return B([build, cursor_key](const Object& body, const std::string& audience) {
      return build(audience, Str(body, "requester_account_id"), Str(body, cursor_key), Timestamp(body));
    });
  };
  const auto device_bytes = [](auto build, const char* last_key) {
    return B([build, last_key](const Object& body, const std::string& audience) {
      return build(audience, Str(body, "account_id"), Str(body, "platform"), Str(body, "device_id"),
                   Str(body, last_key), Timestamp(body));
    });
  };
  // The history request's stream id is the canonical key of the two accounts (checked against the body below).
  const B history_bytes = [history](const Object& body, const std::string& audience) {
    return BuildRelayApiStreamHistorySignBytes(audience, history, Timestamp(body));
  };
  return {
      ApiCall("send", RelayApiOp::Send, "/v1/messages", std::move(send_fields), send_bytes),
      ApiCall("poll_inbox", RelayApiOp::PollInbox, "/v1/inbox/poll", reader("cursor", ""),
              reader_bytes(BuildRelayApiPollInboxSignBytes, "cursor")),
      ApiCall("stream_history", RelayApiOp::StreamHistory, "/v1/streams/messages/query",
              ChatHistoryRequestToStreamHistoryJson(history), history_bytes),
      ApiCall("device_register", RelayApiOp::DeviceRegister, "/v1/devices/register",
              device("push_token", "golden-push-token"),
              device_bytes(BuildRelayApiDeviceRegisterSignBytes, "push_token")),
      ApiCall("device_unregister", RelayApiOp::DeviceUnregister, "/v1/devices/unregister",
              device("binding_id", "golden_binding_01"),
              device_bytes(BuildRelayApiDeviceUnregisterSignBytes, "binding_id")),
      ApiCall("ack_inbox", RelayApiOp::AckInbox, "/v1/inbox/ack", reader("cursor", "golden-cursor"),
              reader_bytes(BuildRelayApiAckInboxSignBytes, "cursor")),
      ApiCall("clear_inbox", RelayApiOp::ClearInbox, "/v1/inbox/clear",
              reader("before_created_at", "2025-10-22T16:00:00.000Z"),
              reader_bytes(BuildRelayApiClearInboxSignBytes, "before_created_at")),
  };
}

std::vector<CallSpec> AllCalls(const VectorKeys& keys) {
  std::vector<CallSpec> calls = RegistryCalls(keys);
  calls.push_back(LockerCall(keys));
  for (auto* group : {&StorageCalls, &MessageCalls}) {
    for (CallSpec& call : (*group)(keys)) {
      calls.push_back(std::move(call));
    }
  }
  return calls;
}

Object Expect(const std::string& at_a, const std::string& at_b) {
  Object expect;
  if (!at_a.empty()) {
    expect.set("a", at_a);
  }
  expect.set("b", at_b);
  return expect;
}

/** One `calls` row: the command signed by the account for relay `a` (bytes), sent with `body_audience`. */
Object CallRow(const VectorKeys& keys, const CallSpec& spec, const std::string& name, const std::string& body_audience,
               Object expect) {
  Object body = spec.fields;
  body.set("timestamp", kNowMs);
  const ByteVector bytes = spec.bytes(body, keys.peer_a);
  if (!body_audience.empty()) {
    body.set("audience", body_audience);
  }
  body.set("signature", Sign(keys, bytes));
  Object row;
  row.set("name", name);
  row.set("domain", spec.domain);
  row.set("version", spec.version);
  if (spec.op >= 0) {
    row.set("op", spec.op);
  }
  row.set("method", std::string("POST"));
  row.set("path", spec.path);
  row.set("body", ObjectValue(std::move(body)));
  row.set("sign_bytes_hex", BytesToHex(bytes));
  row.set("expect", ObjectValue(std::move(expect)));
  return row;
}

Array CallRows(const VectorKeys& keys) {
  Array rows;
  for (const CallSpec& spec : AllCalls(keys)) {
    // register/finish: `b` refuses the audience before the challenge (the relay's handler tests use their own).
    Object expect = spec.name == "register.finish" ? Expect("", "wrong_audience") : Expect("ok", "wrong_audience");
    rows.elements.push_back(ObjectValue(CallRow(keys, spec, spec.name, keys.peer_a, std::move(expect))));
    if (spec.name == "relay-api.poll_inbox") {
      rows.elements.push_back(
          ObjectValue(CallRow(keys, spec, spec.name + ".no_audience", "", Expect("wrong_audience", "wrong_audience"))));
      rows.elements.push_back(ObjectValue(CallRow(keys, spec, spec.name + ".audience_swapped", keys.peer_b,
                                                  Expect("wrong_audience", "bad_signature"))));
    }
  }
  return rows;
}

Object RelayRow(const MlDsaKeyPair& keys, uint8_t seed, const std::string& peer_id, const std::string& origin) {
  Object relay;
  relay.set("seed_hex", BytesToHex(ByteVector(kMlDsa65SeedBytes, seed)));
  relay.set("public_key_b64", Base64Encode(keys.public_key));
  relay.set("peer_id", peer_id);
  relay.set("origin", origin);
  return relay;
}

AccountCard VectorCard(const VectorKeys& keys, std::vector<AccountCardRelay> relays) {
  const std::string pk = Base64Encode(keys.account.public_key);
  const std::string kem = Base64Encode(keys.kem.public_key);
  AccountCard card{.keys = KeyCard{.account_id = keys.account_id,
                                   .signing_public_key_b64 = pk,
                                   .kem_public_key_b64 = kem,
                                   .kem_binding_signature_b64 = Sign(keys, KemBindingSignBytes(pk, kem))},
                   .nickname = "alice",
                   .devices = {AccountCardDevice{.peer_id = keys.device_peer_id,
                                                 .signing_public_key_b64 = Base64Encode(keys.device.public_key),
                                                 .kem_public_key_b64 = Base64Encode(keys.device_kem.public_key)}},
                   .relays = std::move(relays),
                   .initiation_floor = 250,
                   .issued_at = kNowMs};
  return SignAccountCard(std::move(card),
                         [&keys](const ByteVector& bytes) -> Roe<std::string> { return Sign(keys, bytes); })
      .value();
}

std::vector<AccountCardRelay> TwoRelays(const VectorKeys& keys) {
  return {{.origin = kOriginA, .peer_id = keys.peer_a}, {.origin = kOriginB, .peer_id = keys.peer_b}};
}

Object RefusedCard(const std::string& name, Object card) {
  Object row;
  row.set("name", name);
  row.set("card", ObjectValue(std::move(card)));
  return row;
}

/** The card's JSON with relay `index`'s `peer_id` key left out. */
Object WithoutPeerId(Object card, size_t index) {
  const Array* relays = card.getArray("relays");
  Array rows = relays ? *relays : Array{};
  if (index < rows.elements.size()) {
    Object relay = *asObject(rows.elements[index]);
    relay.erase("peer_id");
    rows.elements[index] = ObjectValue(std::move(relay));
  }
  card.set("relays", ArrayValue(std::move(rows.elements)));
  return card;
}

Array RefusedCards(const VectorKeys& keys) {
  Object version_3 = AccountCardToJson(VectorCard(keys, TwoRelays(keys)));
  version_3.set("card_version", int64_t{3});
  std::vector<AccountCardRelay> no_peer = TwoRelays(keys);
  no_peer[1].peer_id.clear();
  std::vector<AccountCardRelay> one_peer = TwoRelays(keys);
  one_peer[1].peer_id = keys.peer_a;
  std::vector<AccountCardRelay> not_a_peer = TwoRelays(keys);
  not_a_peer[1].peer_id = "12D3KooWD3eckifWpRn9wQpMG9R9hX3sD158z7EqHWmweQAJU5SA";
  std::vector<AccountCardRelay> five;
  for (uint8_t i = 0; i < 5; ++i) {
    const MlDsaKeyPair relay = KeysOfSeed(static_cast<uint8_t>(0x20 + i));
    five.push_back({.origin = "https://relay-" + std::to_string(i) + ".example/api/relay",
                    .peer_id = pp::PeerIdFromMlDsaPublicKey(relay.public_key).value()});
  }
  Array rows;
  rows.elements.push_back(ObjectValue(RefusedCard("version_3", std::move(version_3))));
  rows.elements.push_back(ObjectValue(
      RefusedCard("relay_without_peer_id", WithoutPeerId(AccountCardToJson(VectorCard(keys, no_peer)), 1))));
  rows.elements.push_back(
      ObjectValue(RefusedCard("two_relays_one_peer_id", AccountCardToJson(VectorCard(keys, one_peer)))));
  rows.elements.push_back(
      ObjectValue(RefusedCard("peer_id_not_an_ml_dsa_peer_id", AccountCardToJson(VectorCard(keys, not_a_peer)))));
  rows.elements.push_back(ObjectValue(RefusedCard("five_relays", AccountCardToJson(VectorCard(keys, five)))));
  return rows;
}

Object CardVector(const VectorKeys& keys) {
  const AccountCard card = VectorCard(keys, TwoRelays(keys));
  const ByteVector bytes = AccountCardSignBytes(card);
  Object vector;
  vector.set("card", ObjectValue(AccountCardToJson(card)));
  vector.set("sign_bytes_len", static_cast<int64_t>(bytes.size()));
  vector.set("sign_bytes_sha256_hex",
             Sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
  return vector;
}

Object BuildVectors() {
  const VectorKeys keys = MakeKeys();
  Object relays;
  relays.set("a", ObjectValue(RelayRow(keys.relay_a, 0x07, keys.peer_a, kOriginA)));
  relays.set("b", ObjectValue(RelayRow(keys.relay_b, 0x08, keys.peer_b, kOriginB)));
  std::vector<Value> fixtures;
  for (const uint8_t fill : {uint8_t{0x42}, uint8_t{0x01}}) {
    Object fixture;
    fixture.set("public_key_fill_byte", BytesToHex(ByteVector{fill}));
    fixture.set("peer_id", pp::PeerIdFromMlDsaPublicKey(ByteVector(kMlDsa65PublicKeyBytes, fill)).value());
    fixtures.push_back(ObjectValue(std::move(fixture)));
  }
  const std::string pk = Base64Encode(keys.account.public_key);
  const std::string kem = Base64Encode(keys.kem.public_key);
  Object account;
  account.set("seed_hex", BytesToHex(ByteVector(kMlDsa65SeedBytes, 0x09)));
  account.set("public_key_b64", pk);
  account.set("kem_public_key_b64", kem);
  account.set("kem_binding_signature_b64", Sign(keys, KemBindingSignBytes(pk, kem)));
  account.set("account_id", keys.account_id);
  Object root;
  root.set("format", std::string(kFormat));
  root.set("now_ms", kNowMs);
  root.set("relays", ObjectValue(std::move(relays)));
  root.set("peer_id_fixtures", ArrayValue(std::move(fixtures)));
  root.set("account", ObjectValue(std::move(account)));
  root.set("calls", ArrayValue(CallRows(keys).elements));
  root.set("account_card", ObjectValue(CardVector(keys)));
  root.set("refused_cards", ArrayValue(RefusedCards(keys).elements));
  return root;
}

/** `value` with every signature left out (ML-DSA signing is randomized): what must be the same on every run. */
Value WithoutSignatures(const Value& value) {
  static const std::set<std::string> kSignatures = {"signature", "signature_b64", "card_signature_b64",
                                                    "kem_binding_signature_b64"};
  if (const Object* object = asObject(value)) {
    Object out;
    for (const auto& [key, item] : object->entries()) {
      if (!kSignatures.contains(key)) {
        out.set(key, WithoutSignatures(item));
      }
    }
    return ObjectValue(std::move(out));
  }
  if (const Array* array = asArray(value)) {
    std::vector<Value> out;
    for (const Value& item : array->elements) {
      out.push_back(WithoutSignatures(item));
    }
    return ArrayValue(std::move(out));
  }
  return value;
}

Object Committed() {
  std::ifstream file(PP_RELAY_SIGN_VECTORS_PATH);
  EXPECT_TRUE(file.good()) << PP_RELAY_SIGN_VECTORS_PATH;
  std::stringstream text;
  text << file.rdbuf();
  return TryParseObject(text.str()).value_or(Object{});
}

const CallSpec* SpecFor(const std::vector<CallSpec>& specs, const std::string& name) {
  for (const CallSpec& spec : specs) {
    if (name == spec.name || name.rfind(spec.name + ".", 0) == 0) {
      return &spec;
    }
  }
  return nullptr;
}

TEST(RelaySignVectorsTest, WriteAFreshFileWhenAsked) {
  const char* out = std::getenv("PP_WRITE_RELAY_SIGN_VECTORS");
  if (out == nullptr || *out == '\0') {
    GTEST_SKIP() << "set PP_WRITE_RELAY_SIGN_VECTORS=<path> to write the vectors";
  }
  std::ofstream file(out);
  file << DumpJson(BuildVectors(), 2) << "\n";
  ASSERT_TRUE(file.good()) << out;
}

// MR007-VEC: the committed file is what this build makes (but for the randomized signatures): a change to any
// builder changes the file, which the relay copies.
TEST(RelaySignVectorsTest, TheCommittedFileIsWhatTheBuildersMake) {
  const Object committed = Committed();
  EXPECT_EQ(committed.getString("format").value_or(""), kFormat);
  EXPECT_EQ(DumpJson(WithoutSignatures(ObjectValue(committed))),
            DumpJson(WithoutSignatures(ObjectValue(BuildVectors()))))
      << "regenerate with PP_WRITE_RELAY_SIGN_VECTORS=" << PP_RELAY_SIGN_VECTORS_PATH;
}

// MR007-VEC / MR007-AUD: every call's bytes are its body's for relay `a`, signed by the account; the body names `a`
// (but the two that test the refusal); every domain of the version table and every relay API op is covered.
TEST(RelaySignVectorsTest, EveryCallsBytesAreItsBodysAndSigned) {
  const Object committed = Committed();
  const VectorKeys keys = MakeKeys();
  const std::vector<CallSpec> specs = AllCalls(keys);
  const Array* calls = committed.getArray("calls");
  ASSERT_NE(calls, nullptr);
  std::set<std::string> domains;
  std::set<int64_t> ops;
  for (const Value& item : calls->elements) {
    const Object* row = asObject(item);
    ASSERT_NE(row, nullptr);
    const std::string name = Str(*row, "name");
    const CallSpec* spec = SpecFor(specs, name);
    ASSERT_NE(spec, nullptr) << name;
    const Object* body = row->getObject("body");
    ASSERT_NE(body, nullptr) << name;
    const ByteVector bytes = spec->bytes(*body, keys.peer_a);
    EXPECT_EQ(BytesToHex(bytes), Str(*row, "sign_bytes_hex")) << name;
    const auto signature = Base64Decode(Str(*body, "signature"));
    ASSERT_TRUE(signature) << name;
    EXPECT_TRUE(MlDsa::Verify(keys.account.public_key, bytes, *signature).valueOr(false)) << name;
    EXPECT_EQ(Timestamp(*body), kNowMs) << name;
    if (name == spec->name) {
      EXPECT_EQ(Str(*body, "audience"), keys.peer_a) << name;
    }
    domains.insert(Str(*row, "domain") + " v" + std::to_string(row->getIf<int64_t>("version").value_or(0)));
    if (auto op = row->getIf<int64_t>("op")) {
      ops.insert(*op);
    }
  }
  const std::set<std::string> table = {"pp-browser:relay-register-v1 v3",      "pp-browser:relay-revoke-v1 v2",
                                       "pp-browser:relay-profile-v1 v2",       "pp-browser:relay-profile-icon-v1 v2",
                                       "pp-browser:relay-heartbeat-v1 v2",     "pp-browser:relay-llm-token-v1 v2",
                                       "pp-browser:relay-locker-put-v1 v2",    "pp-browser:relay-blob-presign-v1 v2",
                                       "pp-browser:relay-blob-retain-v1 v2",   "pp-browser:relay-blob-delete-v1 v2",
                                       "pp-browser:relay-blob-list-v1 v2",     "pp-browser:relay-file-start-v1 v2",
                                       "pp-browser:relay-file-part-v1 v2",     "pp-browser:relay-file-status-v1 v2",
                                       "pp-browser:relay-file-complete-v1 v2", "pp-browser:relay-file-delete-v1 v2",
                                       "pp-browser:relay-file-list-v1 v2",     "pp-browser:relay-api-v1 v3"};
  EXPECT_EQ(domains, table);
  EXPECT_EQ(ops, (std::set<int64_t>{0, 1, 2, 3, 4, 5, 6}));
}

TEST(RelaySignVectorsTest, TheHistoryBodyNamesTheStreamItsBytesSign) {
  const VectorKeys keys = MakeKeys();
  const ChatHistoryRequest request = HistoryRequest(keys.account_id);
  const Object body = ChatHistoryRequestToStreamHistoryJson(request);
  EXPECT_EQ(Str(body, "stream_id"),
            BuildCanonicalRelayStreamKey(request.requester_identity_value, request.peer_identity_value, request.channel,
                                         request.session_epoch));
}

// MR007-ID: the relays' keys come from their seeds and name the PeerIds a node's would (MESH_IDENTITY's fixtures).
TEST(RelaySignVectorsTest, TheRelaysKeysDeriveTheirPeerIds) {
  const Object committed = Committed();
  for (const char* name : {"a", "b"}) {
    const Object* relay = committed.getObject("relays") ? committed.getObject("relays")->getObject(name) : nullptr;
    ASSERT_NE(relay, nullptr) << name;
    const auto seed = HexToBytes(Str(*relay, "seed_hex"));
    ASSERT_TRUE(seed);
    const MlDsaKeyPair keys = MlDsa::GenerateKeyPairFromSeed(*seed).value();
    EXPECT_EQ(Base64Encode(keys.public_key), Str(*relay, "public_key_b64")) << name;
    EXPECT_EQ(pp::PeerIdFromMlDsaPublicKey(keys.public_key).value(), Str(*relay, "peer_id")) << name;
  }
  const Array* fixtures = committed.getArray("peer_id_fixtures");
  ASSERT_NE(fixtures, nullptr);
  ASSERT_EQ(fixtures->elements.size(), 2u);
  EXPECT_EQ(Str(*asObject(fixtures->elements[0]), "peer_id"), "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP");
  EXPECT_EQ(Str(*asObject(fixtures->elements[1]), "peer_id"), "QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie");
  const Object* account = committed.getObject("account");
  ASSERT_NE(account, nullptr);
  EXPECT_TRUE(VerifyKemBinding(Str(*account, "account_id"), Str(*account, "public_key_b64"),
                               Str(*account, "kem_public_key_b64"), Str(*account, "kem_binding_signature_b64")));
}

// MR007-CARD: the vector card (version 4) verifies with its bytes; every refused card is refused.
TEST(RelaySignVectorsTest, TheCardVerifiesAndTheRefusedCardsAreRefused) {
  const Object committed = Committed();
  const Object* vector = committed.getObject("account_card");
  ASSERT_NE(vector, nullptr);
  const Object* card_json = vector->getObject("card");
  ASSERT_NE(card_json, nullptr);
  auto card = AccountCardFromJson(*card_json);
  ASSERT_TRUE(card) << card.error().message;
  EXPECT_TRUE(VerifyAccountCard(*card, kNowMs));
  EXPECT_EQ(card->relays.size(), 2u);
  const ByteVector bytes = AccountCardSignBytes(*card);
  EXPECT_EQ(static_cast<int64_t>(bytes.size()), vector->getIf<int64_t>("sign_bytes_len").value_or(0));
  EXPECT_EQ(Sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())),
            Str(*vector, "sign_bytes_sha256_hex"));

  const Array* refused = committed.getArray("refused_cards");
  ASSERT_NE(refused, nullptr);
  EXPECT_EQ(refused->elements.size(), 5u);
  for (const Value& item : refused->elements) {
    const Object* row = asObject(item);
    ASSERT_NE(row, nullptr);
    const Object* json = row->getObject("card");
    ASSERT_NE(json, nullptr);
    auto parsed = AccountCardFromJson(*json);
    EXPECT_FALSE(parsed && VerifyAccountCard(*parsed, kNowMs)) << Str(*row, "name");
  }
}

} // namespace
} // namespace pbr
