// Cross-implementation vectors for every signed relay call at sign version 4 (release-compatibility SIGNED_FORMATS
// § 2.8, § 6, INVENTORY F5; multi-relay MR007; docs/contracts/SERVICE_ENDPOINTS.md § The audience): each domain and
// relay API op signed by one account for relay `a`, with an empty extension area and with one unknown non-critical
// entry; the refusals a relay answers, in its order (version, audience, parse, algorithm ids, signature, extensions);
// the relays' keys and PeerIds; answer fixtures with unknown keys; an account card and cards that must be refused.
//
// The relay (pp-relay-apis `relay-sign-vectors.json`) copies the committed file byte for byte and reproduces every
// row; a change to either side's bytes changes it here first. `RelayAnswer` below is the relay's check as this file
// states it. PP_WRITE_RELAY_SIGN_VECTORS=<path> writes a fresh file (keys from fixed seeds; ML-DSA signing is hedged,
// so only the signatures differ between runs). Delivery sits above net and people, whose builders it needs.

#include "lib/delivery/HttpRelayClient.h"
#include "lib/delivery/RelayMessageSignPayload.h"

#include "common/ValueJson.h"
#include "common/chat/MessagingJson.h"
#include "common/chat/RelayStreamKey.h"
#include "common/directory/DirectoryJson.h"
#include "foundation/sockets/testing/ScriptedHttpServer.h"
#include "common/tests/VectorFile.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "foundation/crypto/RelayId.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/Sha256Hex.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/TypedBlob.h"
#include "lib/net/ClientCompat.h"
#include "lib/net/HttpLockerClient.h"
#include "lib/net/OrgBackendClientsImpl.h"
#include "lib/net/RegistrationSignPayload.h"
#include "lib/net/RelayApiSignPayload.h"
#include "lib/net/RelayAudience.h"
#include "lib/net/RelayBlobSignPayload.h"
#include "lib/net/RelayIdentity.h"
#include "lib/net/RelaySignBytes.h"
#include "lib/net/testing/FakeRelaySigner.h"
#include "lib/people/AccountCard.h"
#include "lib/people/CardKeys.h"
#include "lib/people/AccountLocker.h"

#include "crypto/PeerId.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

using pp::ExtArea;
using ::pp::kMlDsa65SeedBytes;
using test::VectorFile;
using test::WithoutKeys;

const VectorFile kFile{PP_RELAY_SIGN_VECTORS_PATH, "PP_WRITE_RELAY_SIGN_VECTORS"};
/** The hedged signatures (and the blobs that hold one): compared by verifying, never by their bytes. */
const std::set<std::string> kSignatureKeys = {"signature", "signature_b64", "card_signature_b64",
                                              "kem_binding_signature_b64", "kem_binding_b64"};

constexpr int64_t kNowMs = 1'761'235'200'000;
constexpr char kFormat[] = "pp-browser relay sign vectors v2";
constexpr char kOriginA[] = "https://relay-a.example/api/relay";
constexpr char kOriginB[] = "https://relay-b.example/api/relay";
constexpr char kBlobId[] = "00000000-0000-4000-8000-000000000001";

// What a relay answers (the file's `answers`).
constexpr char kOk[] = "ok";
constexpr char kWrongAudience[] = "wrong_audience";
constexpr char kBadSignature[] = "bad_signature";
constexpr char kClientUpgrade[] = "client_upgrade_required";
constexpr char kRelayUpgrade[] = "relay_upgrade_required";
constexpr char kMalformed[] = "malformed";

/** Extension types no build defines (SIGNED_EXTENSIONS § Extension types: none at release 1). */
constexpr uint16_t kUnknownType = 0x0042;
constexpr uint16_t kUnknownCriticalType = 0x8042;

/** Every key of the vectors, from fixed seeds. */
struct VectorKeys {
  MlDsaKeyPair relay_a;
  MlDsaKeyPair relay_b;
  std::string peer_a;
  std::string peer_b;
  MlDsaKeyPair account;
  HybridKemKeyPair kem;
  std::string account_id;
  ByteVector typed_public_key;
  ByteVector typed_kem_public_key;
  MlDsaKeyPair device;
  HybridKemKeyPair device_kem;
  std::string device_peer_id;
};

MlDsaKeyPair KeysOfSeed(uint8_t fill) {
  return MlDsa::GenerateKeyPairFromSeed(ByteVector(kMlDsa65SeedBytes, fill)).value();
}

const VectorKeys& Keys() {
  static const VectorKeys keys = [] {
    VectorKeys k{.relay_a = KeysOfSeed(0x07), .relay_b = KeysOfSeed(0x08), .account = KeysOfSeed(0x09)};
    k.peer_a = pp::PeerIdFromMlDsaPublicKey(k.relay_a.public_key).value();
    k.peer_b = pp::PeerIdFromMlDsaPublicKey(k.relay_b.public_key).value();
    k.kem = HybridKem::GenerateKeyPairFromSeed(ByteVector(64, 0x0a)).value();
    k.account_id = AccountIdFromMlDsaPublicKey(k.account.public_key).value();
    k.typed_public_key = TypedSigningPublicKey(k.account.public_key).value();
    k.typed_kem_public_key = TypedKemPublicKey(k.kem.public_key).value();
    k.device = KeysOfSeed(0x0b);
    k.device_kem = HybridKem::GenerateKeyPairFromSeed(ByteVector(64, 0x0c)).value();
    k.device_peer_id = pp::PeerIdFromMlDsaPublicKey(k.device.public_key).value();
    return k;
  }();
  return keys;
}

/** The relay ids of relays `a` and `b` in these vectors (32 bytes of their seeds' fill; card v5 names relays by them). */
RelayId RelayIdOfSeed(const uint8_t seed) {
  return RelayId::FromRaw(ByteVector(kRelayIdBytes, seed)).value();
}

/** The account's typed signature over `bytes`, standard base64. */
std::string Sign(const ByteVector& bytes) {
  return Base64Encode(SignTyped(Keys().account.secret_key, bytes).value());
}

/** The account's KEM binding v2 blob over its typed keys (SIGNED_FORMATS § 2.3), standard base64. */
std::string KemBindingB64() {
  const VectorKeys& keys = Keys();
  const ByteVector signature =
      SignTyped(keys.account.secret_key, KemBindingSignBytes(keys.typed_public_key, keys.typed_kem_public_key)).value();
  return Base64Encode(KemBindingBlob(signature));
}

ExtArea OneEntry(const uint16_t type) {
  return ExtArea::FromEntries({{.type = type, .value = {0xca, 0xfe}}}).value();
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

/** The bytes a body asks to be signed for `audience` with `ext` as their area, as the relay's parser builds them. */
using BodyBytes = std::function<ByteVector(const Object& body, const std::string& audience, const ExtArea& ext)>;

/**
 * One signed call: where it goes, the fields of its body besides `sign_version`, `audience`, `timestamp`, `ext_b64`
 * and `signature`, what its domain signs after the audience (`fields`, the file's `domains` table), and its bytes.
 */
struct CallSpec {
  std::string name;
  std::string domain;
  int64_t op = -1;
  std::string path;
  std::string layout;
  Object fields;
  BodyBytes bytes;
};

/** `account_id`, one more string field, then the timestamp: the storage calls' commonest layout. */
CallSpec AccountTextCall(const std::string& name, const char* domain, const std::string& path, const char* key,
                         const std::string& value,
                         std::vector<uint8_t> (*build)(const std::string&, const std::string&, const std::string&,
                                                       int64_t, const ExtArea&)) {
  Object fields;
  fields.set("account_id", Keys().account_id);
  fields.set(key, value);
  return CallSpec{.name = name,
                  .domain = domain,
                  .path = path,
                  .layout = std::string("LenUtf8(account_id) LenUtf8(") + key + ") i64(timestamp)",
                  .fields = std::move(fields),
                  .bytes = [build, key](const Object& body, const std::string& audience, const ExtArea& ext) {
                    return build(audience, Str(body, "account_id"), Str(body, key), Timestamp(body), ext);
                  }};
}

std::vector<CallSpec> RegistryCalls() {
  const VectorKeys& keys = Keys();
  Object finish;
  finish.set("challenge", std::string("golden-challenge"));
  finish.set("public_key", Base64Encode(keys.typed_public_key));
  finish.set("nickname", std::string("alice"));
  finish.set("kem_public_key_b64", Base64Encode(keys.typed_kem_public_key));
  finish.set("kem_binding_b64", KemBindingB64());
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
       .domain = kRelayRegisterDomain,
       .path = "/v1/register/finish",
       .layout = "LenUtf8(challenge) LenBytes(public_key, typed) LenBytes(kem_public_key_b64, typed) i64(timestamp)",
       .fields = std::move(finish),
       .bytes =
           [](const Object& body, const std::string& audience, const ExtArea& ext) {
             return BuildRegistrationSignBytes(audience, Str(body, "challenge"), Str(body, "public_key"),
                                               Str(body, "kem_public_key_b64"), Timestamp(body), ext);
           }},
      {.name = "register.revoke",
       .domain = kRelayRevokeDomain,
       .path = "/v1/register/revoke",
       .layout = "LenUtf8(account_id) LenUtf8(registration_id) i64(timestamp)",
       .fields = std::move(revoke),
       .bytes =
           [](const Object& body, const std::string& audience, const ExtArea& ext) {
             return BuildAccountRevokeSignBytes(audience, Str(body, "account_id"), Str(body, "registration_id"),
                                                Timestamp(body), ext);
           }},
      {.name = "profile.nickname",
       .domain = kRelayProfileDomain,
       .path = "/v1/profile/nickname",
       .layout = "LenUtf8(account_id) LenUtf8(nickname) i64(timestamp)",
       .fields = std::move(nickname),
       .bytes =
           [](const Object& body, const std::string& audience, const ExtArea& ext) {
             return BuildProfileUpdateSignBytes(audience, Str(body, "account_id"), Str(body, "nickname"),
                                                Timestamp(body), ext);
           }},
      {.name = "mesh.heartbeat",
       .domain = kRelayHeartbeatDomain,
       .path = "/v1/mesh/heartbeat",
       .layout = "LenUtf8(account_id) LenUtf8(peer_id) LenUtf8(state) i64(timestamp)",
       .fields = std::move(heartbeat),
       .bytes =
           [](const Object& body, const std::string& audience, const ExtArea& ext) {
             return BuildRelayHeartbeatSignBytes(audience, Str(body, "account_id"), Str(body, "peer_id"),
                                                 Str(body, "state"), Timestamp(body), ext);
           }},
      {.name = "llm.token.renew",
       .domain = kRelayLlmTokenDomain,
       .path = "/api/llm/v1/token/renew",
       .layout = "LenUtf8(account_id) i64(timestamp)",
       .fields = std::move(llm),
       .bytes =
           [](const Object& body, const std::string& audience, const ExtArea& ext) {
             return BuildLlmTokenRenewSignBytes(audience, Str(body, "account_id"), Timestamp(body), ext);
           }},
  };
}

CallSpec LockerCall() {
  const VectorKeys& keys = Keys();
  const ByteVector locker_id(32, 0xab);
  const ByteVector sealed(48, 0x5e);
  // Locker record v2 (its own vectors: lib/people account_locker_vectors.json); the relay half is what is signed here.
  LockerRecord record{.record_version = kLockerRecordVersion,
                      .locker_id = BytesToHex(locker_id),
                      .version = 3,
                      .signing_public_key_b64 = Base64Encode(keys.typed_public_key),
                      .sealed_b64 = Base64Encode(sealed)};
  record.signature_b64 = Base64Encode(
      SignTyped(keys.account.secret_key, account_locker::SignBytes(locker_id, record.version, sealed, ExtArea{}))
          .value()); // the owner half
  Object fields;
  fields.set("account_id", keys.account_id);
  fields.set("locker", ObjectValue(LockerRecordToJson(record)));
  return {.name = "lockers.put",
          .domain = kRelayLockerPutDomain,
          .path = "/v1/lockers/put",
          .layout = "LenUtf8(account_id) LenUtf8(locker.locker_id) u64(locker.version) i64(timestamp)",
          .fields = std::move(fields),
          .bytes = [](const Object& body, const std::string& audience, const ExtArea& ext) {
            const Object* locker = body.getObject("locker");
            return locker ? BuildLockerPutSignBytes(audience, Str(body, "account_id"), Str(*locker, "locker_id"),
                                                    Num(*locker, "version"), Timestamp(body), ext)
                          : ByteVector{};
          }};
}

std::vector<CallSpec> StorageCalls() {
  const std::string& account = Keys().account_id;
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
      {.name = "cdn.presign",
       .domain = kRelayBlobPresignDomain,
       .path = "/v1/cdn/presign",
       .layout = "LenUtf8(account_id) LenUtf8(content_type) u64(byte_length) LenUtf8(purpose) i64(timestamp)",
       .fields = std::move(presign),
       .bytes =
           [](const Object& body, const std::string& audience, const ExtArea& ext) {
             return BuildBlobPresignSignBytes(audience, Str(body, "account_id"), Str(body, "content_type"),
                                              Num(body, "byte_length"), Str(body, "purpose"), Timestamp(body), ext);
           }},
      {.name = "profile.icon",
       .domain = kRelayProfileIconDomain,
       .path = "/v1/profile/icon",
       .layout = "LenUtf8(account_id) LenUtf8(url) LenUtf8(blob_id) LenUtf8(kind) i64(timestamp)",
       .fields = std::move(icon),
       .bytes =
           [](const Object& body, const std::string& audience, const ExtArea& ext) {
             return BuildProfileIconSignBytes(audience, Str(body, "account_id"), Str(body, "url"), Str(body, "blob_id"),
                                              Str(body, "kind"), Timestamp(body), ext);
           }},
      {.name = "blob.start",
       .domain = kRelayFileStartDomain,
       .path = "/v1/blob/start",
       .layout = "LenUtf8(account_id) u64(byte_length) i64(timestamp)",
       .fields = std::move(start),
       .bytes =
           [](const Object& body, const std::string& audience, const ExtArea& ext) {
             return BuildFileStartSignBytes(audience, Str(body, "account_id"), Num(body, "byte_length"),
                                            Timestamp(body), ext);
           }},
      {.name = "blob.part",
       .domain = kRelayFilePartDomain,
       .path = "/v1/blob/part",
       .layout = "LenUtf8(account_id) LenUtf8(blob_id) u64(part_number) i64(timestamp)",
       .fields = std::move(part),
       .bytes =
           [](const Object& body, const std::string& audience, const ExtArea& ext) {
             return BuildFilePartSignBytes(audience, Str(body, "account_id"), Str(body, "blob_id"),
                                           Num(body, "part_number"), Timestamp(body), ext);
           }},
  };
  calls.push_back(AccountTextCall("cdn.retain", kRelayBlobRetainDomain, "/v1/cdn/retain", "blob_id", kBlobId,
                                  BuildBlobRetainSignBytes));
  calls.push_back(AccountTextCall("cdn.delete", kRelayBlobDeleteDomain, "/v1/cdn/delete", "blob_id", kBlobId,
                                  BuildBlobDeleteSignBytes));
  calls.push_back(AccountTextCall("blob.status", kRelayFileStatusDomain, "/v1/blob/status", "blob_id", kBlobId,
                                  BuildFileStatusSignBytes));
  calls.push_back(AccountTextCall("blob.complete", kRelayFileCompleteDomain, "/v1/blob/complete", "blob_id", kBlobId,
                                  BuildFileCompleteSignBytes));
  calls.push_back(AccountTextCall("blob.delete", kRelayFileDeleteDomain, "/v1/blob/delete", "blob_id", kBlobId,
                                  BuildFileDeleteSignBytes));
  calls.push_back(
      AccountTextCall("blob.list", kRelayFileListDomain, "/v1/blob/list", "status", "pending", BuildFileListSignBytes));
  return calls;
}

/** A relay API call (`pp-browser:relay-api`): the op and the timestamp come before the op's fields. */
CallSpec ApiCall(const std::string& name, RelayApiOp op, const std::string& path, const std::string& layout,
                 Object fields, BodyBytes bytes) {
  return CallSpec{.name = "relay-api." + name,
                  .domain = kRelayApiDomain,
                  .op = static_cast<int64_t>(op),
                  .path = path,
                  .layout = "u8(op) i64(timestamp) " + layout,
                  .fields = std::move(fields),
                  .bytes = std::move(bytes)};
}

ChatHistoryRequest HistoryRequest() {
  ChatHistoryRequest request;
  request.requester_identity_kind = "account";
  request.requester_identity_value = Keys().account_id;
  request.peer_identity_kind = "account";
  request.peer_identity_value = "account:rXRl8_CuSzpM1FI15SM4hzrcy5NjJOp2fdmeA6mdmL0";
  request.channel = ThreadChannel::E2ePublic;
  request.session_epoch = 1;
  request.min_sender_seq = 4;
  request.limit = 50;
  request.order = "desc";
  return request;
}

Object ReaderFields(const char* cursor_key, const std::string& cursor) {
  Object fields;
  fields.set("requester_account_id", Keys().account_id);
  fields.set(cursor_key, cursor);
  return fields;
}

Object DeviceFields(const char* last_key, const std::string& last) {
  Object fields;
  fields.set("account_id", Keys().account_id);
  fields.set("platform", std::string("android"));
  fields.set("device_id", std::string("golden-device"));
  fields.set(last_key, last);
  return fields;
}

using ReaderBuild = std::vector<uint8_t> (*)(const std::string&, const std::string&, const std::string&, int64_t,
                                             const ExtArea&);
using DeviceBuild = std::vector<uint8_t> (*)(const std::string&, const std::string&, const std::string&,
                                             const std::string&, const std::string&, int64_t, const ExtArea&);

BodyBytes ReaderBytes(ReaderBuild build, const char* cursor_key) {
  return [build, cursor_key](const Object& body, const std::string& audience, const ExtArea& ext) {
    return build(audience, Str(body, "requester_account_id"), Str(body, cursor_key), Timestamp(body), ext);
  };
}

BodyBytes DeviceBytes(DeviceBuild build, const char* last_key) {
  return [build, last_key](const Object& body, const std::string& audience, const ExtArea& ext) {
    return build(audience, Str(body, "account_id"), Str(body, "platform"), Str(body, "device_id"), Str(body, last_key),
                 Timestamp(body), ext);
  };
}

std::vector<CallSpec> MessageCalls() {
  const VectorKeys& keys = Keys();
  RelayWireSendRecord send;
  send.sender_account_id = keys.account_id;
  send.recipient_account_id = keys.account_id; // a copy for our own other devices: the account is registered there
  send.stream_id = "v2:e2e_public:" + keys.account_id + ":" + keys.account_id + ":from:" + keys.device_peer_id;
  send.index_key = 7;
  send.blob_b64 = "AA==";
  Object send_fields = RelayWireSendRecordToJson(send);
  for (const char* signed_key : {"sign_version", "audience", "timestamp", "signature"}) {
    send_fields.erase(signed_key);
  }
  const BodyBytes send_bytes = [](const Object& body, const std::string& audience, const ExtArea& ext) {
    RelayWireSendRecord record;
    record.sender_account_id = Str(body, "sender_account_id");
    record.recipient_account_id = Str(body, "recipient_account_id");
    record.stream_id = Str(body, "stream_id");
    record.index_key = Num(body, "index_key");
    return BuildRelayApiSendSignBytes(audience, record, Timestamp(body), ext);
  };
  // The history request's stream id is the canonical key of the two accounts (checked against the body below).
  const BodyBytes history_bytes = [](const Object& body, const std::string& audience, const ExtArea& ext) {
    return BuildRelayApiStreamHistorySignBytes(audience, HistoryRequest(), Timestamp(body), ext);
  };
  return {
      ApiCall("send", RelayApiOp::Send, "/v1/messages",
              "LenUtf8(sender_account_id) LenUtf8(recipient_account_id) LenUtf8(stream_id) u64(index_key)",
              std::move(send_fields), send_bytes),
      ApiCall("poll_inbox", RelayApiOp::PollInbox, "/v1/inbox/poll", "LenUtf8(requester_account_id) LenUtf8(cursor)",
              ReaderFields("cursor", ""), ReaderBytes(BuildRelayApiPollInboxSignBytes, "cursor")),
      ApiCall("stream_history", RelayApiOp::StreamHistory, "/v1/streams/messages/query",
              "LenUtf8(requester_account_id) LenUtf8(sender_account_id) LenUtf8(stream_id) u8(flags: 1 min, 2 max) "
              "[u64(min_index_key)] [u64(max_index_key)] u64(limit) u8(order: 0 asc, 1 desc)",
              ChatHistoryRequestToStreamHistoryJson(HistoryRequest()), history_bytes),
      ApiCall("device_register", RelayApiOp::DeviceRegister, "/v1/devices/register",
              "LenUtf8(account_id) LenUtf8(platform) LenUtf8(device_id) LenUtf8(push_token)",
              DeviceFields("push_token", "golden-push-token"),
              DeviceBytes(BuildRelayApiDeviceRegisterSignBytes, "push_token")),
      ApiCall("device_unregister", RelayApiOp::DeviceUnregister, "/v1/devices/unregister",
              "LenUtf8(account_id) LenUtf8(platform) LenUtf8(device_id) LenUtf8(binding_id)",
              DeviceFields("binding_id", "golden_binding_01"),
              DeviceBytes(BuildRelayApiDeviceUnregisterSignBytes, "binding_id")),
      ApiCall("ack_inbox", RelayApiOp::AckInbox, "/v1/inbox/ack", "LenUtf8(requester_account_id) LenUtf8(cursor)",
              ReaderFields("cursor", "golden-cursor"), ReaderBytes(BuildRelayApiAckInboxSignBytes, "cursor")),
      ApiCall("clear_inbox", RelayApiOp::ClearInbox, "/v1/inbox/clear",
              "LenUtf8(requester_account_id) LenUtf8(before_created_at)",
              ReaderFields("before_created_at", "2025-10-22T16:00:00.000Z"),
              ReaderBytes(BuildRelayApiClearInboxSignBytes, "before_created_at")),
  };
}

const std::vector<CallSpec>& AllCalls() {
  static const std::vector<CallSpec> calls = [] {
    std::vector<CallSpec> all = RegistryCalls();
    all.push_back(LockerCall());
    for (auto* group : {&StorageCalls, &MessageCalls}) {
      for (CallSpec& call : (*group)()) {
        all.push_back(std::move(call));
      }
    }
    return all;
  }();
  return calls;
}

const CallSpec& Spec(const std::string& name) {
  for (const CallSpec& spec : AllCalls()) {
    if (spec.name == name) {
      return spec;
    }
  }
  ADD_FAILURE() << "no call " << name;
  return AllCalls().front();
}

/** The spec a row's name starts with (`profile.nickname.sign_version_5` → `profile.nickname`). */
const CallSpec* SpecOfRow(const std::string& row_name) {
  const CallSpec* best = nullptr;
  for (const CallSpec& spec : AllCalls()) {
    if ((row_name == spec.name || row_name.starts_with(spec.name + ".")) &&
        (best == nullptr || spec.name.size() > best->name.size())) {
      best = &spec;
    }
  }
  return best;
}

// ---- The relay's check, as the file states it (`order`) --------------------------------------------------------

/** One typed blob's read, as an answer: nullopt when it reads. */
std::optional<std::string> AlgorithmAnswer(const TypedRead& read) {
  if (read.status == TypedBlobStatus::UnknownAlgorithm) {
    return std::string(kRelayUpgrade);
  }
  if (!read.ok()) {
    return std::string(kMalformed);
  }
  return std::nullopt;
}

/**
 * What relay `relay_peer` answers `body` for `spec`'s call (SIGNED_FORMATS § 2.8, § 1.1): the sign version from the
 * body first (none or older → client_upgrade_required, newer → relay_upgrade_required, before anything else); the
 * audience (absent or another relay's → wrong_audience); the fields and the area (malformed); the typed blobs' ids
 * (unknown → relay_upgrade_required); the signature over the bytes built with the relay's own PeerId (bad_signature);
 * an unknown critical entry (relay_upgrade_required, after the signature). Unknown body keys and unknown non-critical
 * entries are ignored. The key is the account's (registered there), or for register/finish the body's own.
 */
std::string RelayAnswer(const CallSpec& spec, const Object& body, const std::string& relay_peer) {
  if (!body.contains("sign_version") || body.isNull("sign_version")) {
    return kClientUpgrade;
  }
  const std::optional<int64_t> version = body.getIf<int64_t>("sign_version");
  if (!version) {
    return kMalformed;
  }
  if (*version >= 1 && *version < kRelaySignVersion) {
    return kClientUpgrade;
  }
  if (*version > kRelaySignVersion) {
    return kRelayUpgrade;
  }
  if (*version != kRelaySignVersion) {
    return kMalformed;
  }
  if (Str(body, "audience") != relay_peer) {
    return kWrongAudience;
  }
  auto ext = ReadExtB64(body);
  if (!ext) {
    return kMalformed;
  }
  const bool registering = spec.domain == kRelayRegisterDomain;
  const TypedRead key = registering
                            ? ReadTypedBlobB64(pp::BlobKind::kSignaturePublicKey, Str(body, "public_key"), "public_key")
                            : ReadTypedBlob(pp::BlobKind::kSignaturePublicKey, Keys().typed_public_key, "account key");
  const TypedRead signature = ReadTypedBlobB64(pp::BlobKind::kSignature, Str(body, "signature"), "signature");
  std::vector<TypedRead> reads{key, signature};
  if (registering) {
    reads.push_back(ReadTypedBlobB64(pp::BlobKind::kKemPublicKey, Str(body, "kem_public_key_b64"), "KEM key"));
  }
  if (body.contains("sender_public_key_b64")) {
    reads.push_back(
        ReadTypedBlobB64(pp::BlobKind::kSignaturePublicKey, Str(body, "sender_public_key_b64"), "sender key"));
  }
  for (const TypedRead& read : reads) {
    if (auto answer = AlgorithmAnswer(read)) {
      return *answer;
    }
  }
  const ByteVector bytes = spec.bytes(body, relay_peer, *ext);
  if (bytes.empty()) {
    return kMalformed;
  }
  if (!VerifyTyped(key, bytes, signature)) {
    return kBadSignature;
  }
  if (ext->HasUnknownCritical({})) {
    return kRelayUpgrade;
  }
  return kOk;
}

// ---- Rows -------------------------------------------------------------------------------------------------------

Object Expect(const std::string& at_a, const std::string& at_b) {
  Object expect;
  expect.set("a", at_a);
  expect.set("b", at_b);
  return expect;
}

/** How one row differs from its call signed plainly for relay `a`. */
struct RowCase {
  std::string suffix; // empty: the plain call
  std::string at_a = kOk;
  std::string at_b = kWrongAudience;
  std::string why;
  /** The area the account signs (and the body names, unless `after_sign` changes it). */
  ExtArea signed_ext;
  /** Edits to the fields before signing (they are signed: an unknown body key, the sender's key). */
  std::function<void(Object&)> before_sign;
  /** Edits after signing: what the relay must refuse (or ignore). */
  std::function<void(Object&)> after_sign;
};

Object CallRow(const CallSpec& spec, const RowCase& row_case) {
  const VectorKeys& keys = Keys();
  Object body = spec.fields;
  if (row_case.before_sign) {
    row_case.before_sign(body);
  }
  body.set("timestamp", kNowMs);
  const ByteVector bytes = spec.bytes(body, keys.peer_a, row_case.signed_ext);
  EXPECT_FALSE(bytes.empty()) << spec.name << row_case.suffix;
  body.set("sign_version", static_cast<int64_t>(kRelaySignVersion));
  body.set("audience", keys.peer_a);
  WriteExtB64(body, row_case.signed_ext);
  body.set("signature", Sign(bytes));
  if (row_case.after_sign) {
    row_case.after_sign(body);
  }
  Object row;
  row.set("name", row_case.suffix.empty() ? spec.name : spec.name + "." + row_case.suffix);
  row.set("domain", spec.domain);
  row.set("version", static_cast<int64_t>(kRelaySignVersion));
  if (spec.op >= 0) {
    row.set("op", spec.op);
  }
  row.set("method", std::string("POST"));
  row.set("path", spec.path);
  row.set("body", ObjectValue(std::move(body)));
  row.set("sign_bytes_hex", BytesToHex(bytes));
  row.set("expect", ObjectValue(Expect(row_case.at_a, row_case.at_b)));
  if (!row_case.why.empty()) {
    row.set("why", row_case.why);
  }
  return row;
}

/** `b64`'s first byte (a typed blob's algorithm id) set to `id`. */
std::string WithAlgId(const std::string& b64, const uint8_t id) {
  ByteVector bytes = Base64Decode(b64).valueOr(ByteVector{});
  if (!bytes.empty()) {
    bytes[0] = id;
  }
  return Base64Encode(bytes);
}

/** The refusals every relay checks the same way, on one call of each kind (`refused` rows). */
/** The field a refusal edits after signing: one the relay's account lookup does not read. */
std::string EditedField(const CallSpec& spec) {
  if (spec.domain == kRelayRegisterDomain) {
    return "challenge";
  }
  return spec.domain == kRelayApiDomain ? "cursor" : "nickname";
}

std::vector<RowCase> RefusalCases(const CallSpec& spec) {
  const std::string edited = EditedField(spec);
  std::vector<RowCase> cases{
      {.suffix = "sign_version_3",
       .at_a = kClientUpgrade,
       .at_b = kClientUpgrade,
       .why = "read first, from the body alone: an older app (426), nothing else read",
       .after_sign = [](Object& body) { body.set("sign_version", int64_t{3}); }},
      {.suffix = "no_sign_version",
       .at_a = kClientUpgrade,
       .at_b = kClientUpgrade,
       .why = "none named: an older app (426); there is no legacy window",
       .after_sign = [](Object& body) { body.erase("sign_version"); }},
      {.suffix = "sign_version_5",
       .at_a = kRelayUpgrade,
       .at_b = kRelayUpgrade,
       .why = "a newer app: 400 relay_upgrade_required, before the audience; never a bad signature",
       .after_sign = [](Object& body) { body.set("sign_version", int64_t{5}); }},
      {.suffix = "no_audience",
       .at_a = kWrongAudience,
       .at_b = kWrongAudience,
       .why = "the audience is checked after the version, before anything is spent",
       .after_sign = [](Object& body) { body.erase("audience"); }},
      {.suffix = "audience_swapped",
       .at_a = kWrongAudience,
       .at_b = kBadSignature,
       .why = "signed for a, sent naming b: b rebuilds the bytes with its own PeerId",
       .after_sign = [](Object& body) { body.set("audience", Keys().peer_b); }},
      {.suffix = "unknown_signature_algorithm",
       .at_a = kRelayUpgrade,
       .why = "the signature's algorithm id 2: unknown, update needed (after the audience), not verified",
       .after_sign = [](Object& body) { body.set("signature", WithAlgId(Str(body, "signature"), 2)); }},
      {.suffix = "malformed_signature",
       .at_a = kMalformed,
       .why = "algorithm id 0: refused as malformed, not verified",
       .after_sign = [](Object& body) { body.set("signature", WithAlgId(Str(body, "signature"), 0)); }},
      {.suffix = "unknown_critical_entry",
       .at_a = kRelayUpgrade,
       .why = "it verifies, then the critical entry it does not know: update needed, not acted on",
       .signed_ext = OneEntry(kUnknownCriticalType)},
      {.suffix = "unsigned_critical_entry_added",
       .at_a = kBadSignature,
       .why = "the area is in the signed bytes: an entry added after signing fails the signature, never says update",
       .after_sign = [](Object& body) { WriteExtB64(body, OneEntry(kUnknownCriticalType)); }},
      {.suffix = "unsigned_entry_added",
       .at_a = kBadSignature,
       .why = "the same for a non-critical entry",
       .after_sign = [](Object& body) { WriteExtB64(body, OneEntry(kUnknownType)); }},
      {.suffix = "malformed_area",
       .at_a = kMalformed,
       .why = "ext_b64 of three bytes: no entry (SIGNED_EXTENSIONS § The extension area)",
       .after_sign = [](Object& body) { body.set(kExtB64Key, Base64Encode(ByteVector{0x00, 0x42, 0x00})); }},
      {.suffix = "field_edited",
       .at_a = kBadSignature,
       .why = "the signature covers every field",
       .after_sign = [edited](Object& body) { body.set(edited, Str(body, edited.c_str()) + "-edited"); }},
      {.suffix = "unknown_body_key",
       .why = "unknown body keys are ignored",
       .before_sign = [](Object& body) { body.set("future_field", std::string("ignored")); }},
  };
  if (spec.domain == kRelayRegisterDomain) {
    cases.push_back(
        {.suffix = "unknown_key_algorithm",
         .at_a = kRelayUpgrade,
         .why = "the signing key's algorithm id 2: update needed, not verified",
         .after_sign = [](Object& body) { body.set("public_key", WithAlgId(Str(body, "public_key"), 2)); }});
    cases.push_back({.suffix = "unknown_kem_algorithm",
                     .at_a = kRelayUpgrade,
                     .why = "the KEM key's algorithm id 2: update needed, not verified",
                     .after_sign = [](Object& body) {
                       body.set("kem_public_key_b64", WithAlgId(Str(body, "kem_public_key_b64"), 2));
                     }});
  }
  return cases;
}

/** The calls each refusal runs on: one of the registry's own (register), one plain domain, one relay API op. */
const std::vector<std::string> kRefusalCalls = {"register.finish", "profile.nickname", "relay-api.poll_inbox"};

Array CallRows() {
  Array rows;
  for (const CallSpec& spec : AllCalls()) {
    rows.elements.push_back(ObjectValue(CallRow(spec, RowCase{})));
    rows.elements.push_back(
        ObjectValue(CallRow(spec, RowCase{.suffix = "unknown_non_critical_entry",
                                          .why = "signed with one entry no build knows: ignored, the call accepted",
                                          .signed_ext = OneEntry(kUnknownType)})));
    if (spec.name == "relay-api.send") {
      rows.elements.push_back(ObjectValue(CallRow(
          spec, RowCase{.suffix = "with_sender_key",
                        .why = "a send from an account not registered there carries its typed key (unsigned; its raw "
                               "key hashes to sender_account_id)",
                        .before_sign = [](Object& body) {
                          body.set("sender_public_key_b64", Base64Encode(Keys().typed_public_key));
                        }})));
    }
  }
  for (const std::string& name : kRefusalCalls) {
    const CallSpec& spec = Spec(name);
    for (const RowCase& row_case : RefusalCases(spec)) {
      rows.elements.push_back(ObjectValue(CallRow(spec, row_case)));
    }
  }
  return rows;
}

// ---- Tables the relay reads -------------------------------------------------------------------------------------

Object Layout() {
  Object layout;
  layout.set("header", std::string("domain 00 u8(4) LenUtf8(audience)"));
  layout.set("relay_api", std::string("\"pp-browser:relay-api\" 00 u8(4) LenUtf8(audience) u8(op) i64(timestamp) "
                                      "...the op's fields... u64(len(entries)) entries"));
  layout.set("signed_bytes", std::string("header || the domain's fields (`domains`) || u64(len(entries)) entries"));
  layout.set("encodings", std::string("LenUtf8 / LenBytes: u64 BE byte length, then the bytes; u64, i64 big endian; "
                                      "typed: LenBytes(u8 algorithm id || the algorithm's bytes)"));
  layout.set("body", std::string("the fields, sign_version (4), audience (the relay's transport PeerId), timestamp, "
                                 "ext_b64 (standard base64 of the entries; left out when empty), signature (typed); "
                                 "no signature_alg"));
  return layout;
}

Array DomainRows() {
  Array rows;
  for (const CallSpec& spec : AllCalls()) {
    Object row;
    row.set("domain", spec.domain);
    if (spec.op >= 0) {
      row.set("op", spec.op);
    }
    row.set("call", spec.name);
    row.set("path", spec.path);
    row.set("fields", spec.layout);
    rows.elements.push_back(ObjectValue(std::move(row)));
  }
  return rows;
}

Object Answers() {
  Object answers;
  answers.set(kOk, std::string("2xx: the call is accepted"));
  answers.set(kClientUpgrade, std::string("426 code client_upgrade_required: sign_version none or older"));
  answers.set(kRelayUpgrade, std::string("400 code relay_upgrade_required: a newer sign_version, an unknown "
                                         "algorithm id, an unknown critical entry (after the signature)"));
  answers.set(kWrongAudience, std::string("401 code wrong_audience: audience absent or not this relay's PeerId"));
  answers.set(kBadSignature, std::string("401: the signature does not verify over the rebuilt bytes"));
  answers.set(kMalformed,
              std::string("400, any code but the upgrade ones: a field, the area or a typed blob malformed"));
  Array order;
  for (const char* step : {"sign_version (body alone)", "audience", "parse the fields and ext_b64",
                           "typed blobs' algorithm ids", "signature", "critical extension entries"}) {
    order.elements.push_back(Value(std::string(step)));
  }
  answers.set("order", ArrayValue(std::move(order.elements)));
  return answers;
}

// ---- Answer fixtures (F5): what the app reads, unknown keys beside it ---------------------------------------------

Object Fixture(const std::string& name, const std::string& path, Object answer, const std::string& why) {
  answer.set("future_key", ObjectValue([] {
               Object future;
               future.set("added_later", true);
               return future;
             }()));
  Object row;
  row.set("name", name);
  row.set("path", path);
  row.set("answer", ObjectValue(std::move(answer)));
  row.set("why", why);
  return row;
}

Array ResponseRows() {
  const VectorKeys& keys = Keys();
  Object finish;
  finish.set("success", true);
  finish.set("account_id", keys.account_id);
  finish.set("registration_id", std::string("golden_registration_01"));
  finish.set("message", std::string("Registered"));
  finish.set("expires_at", std::string("2026-11-20T00:00:00.000Z"));
  finish.set("llm_token", std::string("brf_llm_golden"));
  finish.set("llm_token_expires_at", std::string("2025-11-22T16:00:00.000Z"));
  finish.set("kem_bound", true);
  finish.set("account_card_status", std::string("stored"));
  Object poll;
  poll.set("messages", ArrayValue(std::vector<Value>{}));
  poll.set("next_cursor", std::string("golden-cursor-2"));
  poll.set("server_time", kNowMs + 123);
  Object by_account;
  by_account.set("account_id", keys.account_id);
  by_account.set("nickname", std::string("alice"));
  by_account.set("signing_public_key_b64", Base64Encode(keys.typed_public_key));
  by_account.set("kem_public_key_b64", Base64Encode(keys.typed_kem_public_key));
  by_account.set("kem_binding_b64", KemBindingB64());
  Object compat;
  compat.set("schema_version", int64_t{1});
  compat.set("min_client_version", std::string("1.0.0"));
  compat.set("latest_client_version", std::string("1.0.0"));
  compat.set("relay_certificate_b64", std::string("AQ=="));
  Object identity;
  identity.set("schema_version", kRelayIdentitySchemaVersion);
  identity.set("relay_id", std::string("relay:lelZMZgWuhoMQTvLLUT1C5qlJBPX66oxOLzIT_VsEs4"));
  identity.set("certificate_b64", std::string("AQ=="));
  Array rows;
  rows.elements.push_back(ObjectValue(Fixture("register.finish", "/v1/register/finish", std::move(finish),
                                              "the fields the app reads; no signature_alg; unknown keys ignored")));
  rows.elements.push_back(ObjectValue(Fixture("inbox.poll", "/v1/inbox/poll", std::move(poll),
                                              "messages, next_cursor, server_time read; unknown keys ignored")));
  rows.elements.push_back(ObjectValue(
      Fixture("users.by_account", "/v1/users/by-account/{account_id}", std::move(by_account),
              "the directory row: typed keys and the KEM binding v2 blob, no signature_alg; the account card (v5, "
              "account_card_vectors.json) is left out here")));
  rows.elements.push_back(ObjectValue(Fixture("client_compat", "/v1/client-compat", std::move(compat),
                                              "schema_version 1, additive: relay_certificate_b64 read as it came, "
                                              "checked by the pins (relay_certificate_vectors.json)")));
  rows.elements.push_back(ObjectValue(Fixture("relay_identity", "/v1/relay-identity", std::move(identity),
                                              "v2: relay_id and certificate_b64 (checked by the pins, not here); no "
                                              "peer_id, public_key_b64 or signature_alg")));
  return rows;
}

// ---- The account card (v5, cutover W2; account_card_vectors.json holds the whole set) ---------------------------

/** The account's raw ML-DSA-65 signature, base64: what the identity's signer gives (the card types it). */
Roe<std::string> RawSign(const ByteVector& bytes) {
  return Base64Encode(MlDsa::Sign(Keys().account.secret_key, bytes).value());
}

AccountCard VectorCard(std::vector<AccountCardRelay> relays) {
  const VectorKeys& keys = Keys();
  AccountCard card{.keys = MakeKeyCard(keys.account_id, Base64Encode(keys.account.public_key),
                                       Base64Encode(keys.kem.public_key), RawSign)
                               .value(),
                   .nickname = "alice",
                   .devices = {AccountCardDevice{
                       .peer_id = keys.device_peer_id,
                       .signing_public_key_b64 =
                           TypedKeyB64(pp::BlobKind::kSignaturePublicKey, Base64Encode(keys.device.public_key)).value(),
                       .kem_public_key_b64 =
                           TypedKeyB64(pp::BlobKind::kKemPublicKey, Base64Encode(keys.device_kem.public_key)).value()}},
                   .relays = std::move(relays),
                   .initiation_floor = 250,
                   .issued_at = kNowMs};
  return SignAccountCard(std::move(card), RawSign).value();
}

std::vector<AccountCardRelay> TwoRelays() {
  return {{.origin = kOriginA, .relay_id = RelayIdOfSeed(0x07)}, {.origin = kOriginB, .relay_id = RelayIdOfSeed(0x08)}};
}

Object RefusedCard(const std::string& name, Object card) {
  Object row;
  row.set("name", name);
  row.set("card", ObjectValue(std::move(card)));
  return row;
}

/** The card's JSON with relay `index`'s `relay_id` set to `value`, or left out when it is empty. */
Object WithRelayId(Object card, size_t index, const std::string& value) {
  const Array* relays = card.getArray("relays");
  Array rows = relays ? *relays : Array{};
  if (index < rows.elements.size()) {
    Object relay = *asObject(rows.elements[index]);
    if (value.empty()) {
      relay.erase("relay_id");
    } else {
      relay.set("relay_id", value);
    }
    rows.elements[index] = ObjectValue(std::move(relay));
  }
  card.set("relays", ArrayValue(std::move(rows.elements)));
  return card;
}

Array RefusedCards() {
  Object version_4 = AccountCardToJson(VectorCard(TwoRelays()));
  version_4.set("card_version", int64_t{4});
  std::vector<AccountCardRelay> one_id = TwoRelays();
  one_id[1].relay_id = one_id[0].relay_id;
  std::vector<AccountCardRelay> five;
  for (uint8_t i = 0; i < 5; ++i) {
    five.push_back({.origin = "https://relay-" + std::to_string(i) + ".example/api/relay",
                    .relay_id = RelayIdOfSeed(static_cast<uint8_t>(0x20 + i))});
  }
  const Object card = AccountCardToJson(VectorCard(TwoRelays()));
  Array rows;
  rows.elements.push_back(ObjectValue(RefusedCard("version_4", std::move(version_4))));
  rows.elements.push_back(ObjectValue(RefusedCard("relay_without_relay_id", WithRelayId(card, 1, ""))));
  rows.elements.push_back(ObjectValue(RefusedCard("two_relays_one_relay_id", AccountCardToJson(VectorCard(one_id)))));
  rows.elements.push_back(ObjectValue(RefusedCard("relay_named_by_peer_id", WithRelayId(card, 1, Keys().peer_b))));
  rows.elements.push_back(ObjectValue(RefusedCard("five_relays", AccountCardToJson(VectorCard(five)))));
  return rows;
}

Object CardVector() {
  const AccountCard card = VectorCard(TwoRelays());
  const ByteVector bytes = AccountCardSignBytes(card);
  Object vector;
  vector.set("card", ObjectValue(AccountCardToJson(card)));
  vector.set("sign_bytes_len", static_cast<int64_t>(bytes.size()));
  vector.set("sign_bytes_sha256_hex",
             Sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
  return vector;
}

// ---- The file ---------------------------------------------------------------------------------------------------

Object RelayRow(const MlDsaKeyPair& keys, uint8_t seed, const std::string& peer_id, const std::string& origin) {
  Object relay;
  relay.set("seed_hex", BytesToHex(ByteVector(kMlDsa65SeedBytes, seed)));
  relay.set("public_key_b64", Base64Encode(keys.public_key));
  relay.set("peer_id", peer_id);
  relay.set("relay_id", RelayIdOfSeed(seed).ToString()); // what an account card names it by (card v5)
  relay.set("origin", origin);
  return relay;
}

Object BuildVectors() {
  const VectorKeys& keys = Keys();
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
  Object account;
  account.set("seed_hex", BytesToHex(ByteVector(kMlDsa65SeedBytes, 0x09)));
  account.set("public_key_b64", Base64Encode(keys.typed_public_key));
  account.set("kem_seed_hex", BytesToHex(ByteVector(64, 0x0a)));
  account.set("kem_public_key_b64", Base64Encode(keys.typed_kem_public_key));
  account.set("kem_binding_b64", KemBindingB64());
  account.set("account_id", keys.account_id);
  Object root;
  root.set("format", std::string(kFormat));
  root.set("sign_version", static_cast<int64_t>(kRelaySignVersion));
  root.set("now_ms", kNowMs);
  root.set("layout", ObjectValue(Layout()));
  root.set("domains", ArrayValue(DomainRows().elements));
  root.set("answers", ObjectValue(Answers()));
  root.set("relays", ObjectValue(std::move(relays)));
  root.set("peer_id_fixtures", ArrayValue(std::move(fixtures)));
  root.set("account", ObjectValue(std::move(account)));
  root.set("calls", ArrayValue(CallRows().elements));
  root.set("responses", ArrayValue(ResponseRows().elements));
  root.set("account_card", ObjectValue(CardVector()));
  root.set("refused_cards", ArrayValue(RefusedCards().elements));
  return root;
}

const Object& Committed() {
  static const Object committed = kFile.Read();
  return committed;
}

std::vector<const Object*> Rows(const char* key) {
  std::vector<const Object*> rows;
  if (const Array* array = Committed().getArray(key)) {
    for (const Value& item : array->elements) {
      if (const Object* row = asObject(item)) {
        rows.push_back(row);
      }
    }
  }
  return rows;
}

const Object* Response(const std::string& name) {
  for (const Object* row : Rows("responses")) {
    if (Str(*row, "name") == name) {
      return row->getObject("answer");
    }
  }
  ADD_FAILURE() << "no response fixture " << name;
  return nullptr;
}

TEST(RelaySignVectorsTest, WriteAFreshFileWhenAsked) {
  if (!kFile.WriteRequested()) {
    GTEST_SKIP() << kFile.SkipReason();
  }
  ASSERT_TRUE(kFile.Write(BuildVectors()));
}

// F5: the committed file is what this build makes (but for the hedged signatures): a change to any builder changes
// the file, which the relay copies.
TEST(RelaySignVectorsTest, TheCommittedFileIsWhatTheBuildersMake) {
  EXPECT_EQ(Str(Committed(), "format"), kFormat);
  EXPECT_EQ(VectorFile::Text(WithoutKeys(Committed(), kSignatureKeys)),
            VectorFile::Text(WithoutKeys(BuildVectors(), kSignatureKeys)))
      << kFile.RegenerateHint();
}

// SIGNED_FORMATS § 2.8: every call's bytes are its body's for relay `a` (the area its ext_b64 names), signed by the
// account; the plain rows carry sign_version 4 and name `a`.
TEST(RelaySignVectorsTest, EveryCallsBytesAreItsBodysAndSigned) {
  const VectorKeys& keys = Keys();
  const TypedRead account_key = ReadTypedBlob(pp::BlobKind::kSignaturePublicKey, keys.typed_public_key, "account");
  for (const Object* row : Rows("calls")) {
    const std::string name = Str(*row, "name");
    const CallSpec* spec = SpecOfRow(name);
    ASSERT_NE(spec, nullptr) << name;
    const Object* body = row->getObject("body");
    ASSERT_NE(body, nullptr) << name;
    auto bytes = HexToBytes(Str(*row, "sign_bytes_hex"));
    ASSERT_TRUE(bytes) << name;
    EXPECT_EQ(SignedBytesVersion(*bytes), kRelaySignVersion) << name;
    EXPECT_EQ(Timestamp(*body), kNowMs) << name;
    const std::string signature = Str(*body, "signature");
    // The account signed these bytes (a row whose signature's id was changed after signing: its body under id 1).
    const TypedRead typed = ReadTypedBlobB64(pp::BlobKind::kSignature, WithAlgId(signature, 1), "signature");
    ASSERT_TRUE(typed.ok()) << name;
    EXPECT_TRUE(static_cast<bool>(VerifyTyped(account_key, *bytes, typed))) << name;
    // Rows signed as sent rebuild their bytes from the body (the others were changed after signing).
    const Object* expect = row->getObject("expect");
    ASSERT_NE(expect, nullptr) << name;
    if (Str(*expect, "a") == kOk || name.ends_with(".unknown_critical_entry")) {
      auto ext = ReadExtB64(*body);
      ASSERT_TRUE(ext) << name;
      EXPECT_EQ(BytesToHex(spec->bytes(*body, keys.peer_a, *ext)), Str(*row, "sign_bytes_hex")) << name;
      EXPECT_EQ(Str(*body, "audience"), keys.peer_a) << name;
      EXPECT_EQ(body->getIf<int64_t>("sign_version"), kRelaySignVersion) << name;
      EXPECT_FALSE(body->contains("signature_alg")) << name;
    }
  }
}

// F5 / SIGNED_FORMATS § 2.8: what relays `a` and `b` answer each row, by the relay's check as the file states it.
TEST(RelaySignVectorsTest, EachRelayAnswersWhatTheRowExpects) {
  const VectorKeys& keys = Keys();
  for (const Object* row : Rows("calls")) {
    const std::string name = Str(*row, "name");
    const CallSpec* spec = SpecOfRow(name);
    ASSERT_NE(spec, nullptr) << name;
    const Object* body = row->getObject("body");
    const Object* expect = row->getObject("expect");
    ASSERT_TRUE(body && expect) << name;
    EXPECT_EQ(RelayAnswer(*spec, *body, keys.peer_a), Str(*expect, "a")) << name << " at a";
    EXPECT_EQ(RelayAnswer(*spec, *body, keys.peer_b), Str(*expect, "b")) << name << " at b";
  }
}

// Every domain (17: `blob-list` went with the CDN's list route, relay R4) and every relay API op is covered, each with
// an empty area and with an unknown non-critical entry; the refusals run on one call of each kind.
TEST(RelaySignVectorsTest, EveryDomainAndOpIsCovered) {
  std::set<std::string> domains;
  std::set<int64_t> ops;
  std::set<std::string> names;
  for (const Object* row : Rows("calls")) {
    domains.insert(Str(*row, "domain"));
    if (auto op = row->getIf<int64_t>("op")) {
      ops.insert(*op);
    }
    names.insert(Str(*row, "name"));
  }
  EXPECT_EQ(domains, std::set<std::string>(std::begin(kRelaySignDomains), std::end(kRelaySignDomains)));
  EXPECT_EQ(domains.size(), 17u);
  EXPECT_EQ(ops, (std::set<int64_t>{0, 1, 2, 3, 4, 5, 6}));
  for (const CallSpec& spec : AllCalls()) {
    EXPECT_TRUE(names.contains(spec.name)) << spec.name;
    EXPECT_TRUE(names.contains(spec.name + ".unknown_non_critical_entry")) << spec.name;
  }
  for (const std::string& call : kRefusalCalls) {
    for (const char* refusal : {"sign_version_3", "sign_version_5", "no_sign_version", "unknown_signature_algorithm",
                                "unknown_critical_entry", "unsigned_critical_entry_added", "field_edited"}) {
      EXPECT_TRUE(names.contains(call + "." + refusal)) << call << "." << refusal;
    }
  }
  for (const char* domain : kRelaySignDomains) {
    EXPECT_TRUE(IsSignedDomain(domain)) << domain << ": no -vN suffix";
  }
}

TEST(RelaySignVectorsTest, TheHistoryBodyNamesTheStreamItsBytesSign) {
  const ChatHistoryRequest request = HistoryRequest();
  const Object body = ChatHistoryRequestToStreamHistoryJson(request);
  EXPECT_EQ(Str(body, "stream_id"),
            BuildCanonicalRelayStreamKey(request.requester_identity_value, request.peer_identity_value, request.channel,
                                         request.session_epoch));
}

/** `binding_b64` is the account's KEM binding v2 blob over its typed keys (SIGNED_FORMATS § 2.3). */
bool VerifiesKemBinding(const std::string& binding_b64) {
  const VectorKeys& keys = Keys();
  auto blob = Base64Decode(binding_b64);
  if (!blob) {
    return false;
  }
  SignedBlobReader in(*blob);
  const uint8_t version = in.U8();
  const ExtArea ext = in.Ext();
  const ByteVector signature = in.Bytes();
  return in.Done() && version == kKemBindingVersion &&
         static_cast<bool>(VerifyTyped(ReadTypedBlob(pp::BlobKind::kSignaturePublicKey, keys.typed_public_key, "key"),
                                       KemBindingSignBytes(keys.typed_public_key, keys.typed_kem_public_key, ext),
                                       ReadTypedBlob(pp::BlobKind::kSignature, signature, "binding")));
}

// MR007-ID: the relays' keys come from their seeds and name the PeerIds a node's would (MESH_IDENTITY's fixtures); the
// account's typed keys come from theirs, and its KEM binding v2 verifies.
TEST(RelaySignVectorsTest, TheKeysDeriveFromTheirSeeds) {
  for (const char* name : {"a", "b"}) {
    const Object* relay = Committed().getObject("relays") ? Committed().getObject("relays")->getObject(name) : nullptr;
    ASSERT_NE(relay, nullptr) << name;
    auto seed = HexToBytes(Str(*relay, "seed_hex"));
    ASSERT_TRUE(seed);
    const MlDsaKeyPair keys = MlDsa::GenerateKeyPairFromSeed(*seed).value();
    EXPECT_EQ(Base64Encode(keys.public_key), Str(*relay, "public_key_b64")) << name;
    EXPECT_EQ(pp::PeerIdFromMlDsaPublicKey(keys.public_key).value(), Str(*relay, "peer_id")) << name;
  }
  const std::vector<const Object*> fixtures = Rows("peer_id_fixtures");
  ASSERT_EQ(fixtures.size(), 2u);
  EXPECT_EQ(Str(*fixtures[0], "peer_id"), "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP");
  EXPECT_EQ(Str(*fixtures[1], "peer_id"), "QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie");

  const Object* account = Committed().getObject("account");
  ASSERT_NE(account, nullptr);
  auto seed = HexToBytes(Str(*account, "seed_hex"));
  ASSERT_TRUE(seed);
  const MlDsaKeyPair keys = MlDsa::GenerateKeyPairFromSeed(*seed).value();
  EXPECT_EQ(Str(*account, "public_key_b64"), Base64Encode(TypedSigningPublicKey(keys.public_key).value()));
  EXPECT_EQ(Str(*account, "account_id"), AccountIdFromMlDsaPublicKey(keys.public_key).value())
      << "the Account ID hashes the raw key";
  EXPECT_EQ(Str(*account, "kem_public_key_b64"),
            Base64Encode(TypedKemPublicKey(HybridKem::GenerateKeyPairFromSeed(
                                               HexToBytes(Str(*account, "kem_seed_hex")).valueOr(ByteVector{}))
                                               .value()
                                               .public_key)
                             .value()));
  EXPECT_TRUE(VerifiesKemBinding(Str(*account, "kem_binding_b64")));
  for (const Object* row : Rows("calls")) {
    if (const Object* body = row->getObject("body"); body && body->contains("kem_binding_b64")) {
      EXPECT_TRUE(VerifiesKemBinding(Str(*body, "kem_binding_b64"))) << Str(*row, "name");
    }
  }
  if (const Object* by_account = Response("users.by_account")) {
    EXPECT_TRUE(VerifiesKemBinding(Str(*by_account, "kem_binding_b64")));
  }
}

// F5: the app reads each answer fixture, unknown keys beside its fields.
TEST(RelaySignVectorsTest, TheAppReadsEachAnswerFixture) {
  const VectorKeys& keys = Keys();
  const Object* identity = Response("relay_identity");
  ASSERT_NE(identity, nullptr);
  auto parsed_identity = ParseRelayIdentity(DumpJson(*identity));
  ASSERT_TRUE(parsed_identity) << parsed_identity.error().message;
  EXPECT_EQ(parsed_identity->certificate_b64, "AQ==");

  const Object* compat = Response("client_compat");
  ASSERT_NE(compat, nullptr);
  auto parsed_compat = ParseClientCompatDocument(DumpJson(*compat));
  ASSERT_TRUE(parsed_compat) << parsed_compat.error().message;
  EXPECT_EQ(parsed_compat->relay_certificate_b64, "AQ==");

  const Object* row = Response("users.by_account");
  ASSERT_NE(row, nullptr);
  const mesh::DirectoryHit hit = DirectoryHitFromJson(*row);
  EXPECT_EQ(hit.nickname, "alice");
  EXPECT_EQ(hit.signing_public_key_b64.value_or(""), Base64Encode(keys.typed_public_key));
  EXPECT_EQ(hit.kem_binding_b64.value_or(""), Str(*row, "kem_binding_b64"));
}

/** A loopback relay that answers every request with `answer`. */
class AnswerFixtureTest : public ::testing::Test {
protected:
  void Answer(const Object& answer) {
    const std::string text = DumpJson(answer);
    server_.SetHandler([text](const test::ScriptedHttpServer::Request&) {
      return test::ScriptedHttpServer::Reply{.status = 200, .headers = {}, .body = text};
    });
  }
  test::ScriptedHttpServer server_;
};

TEST_F(AnswerFixtureTest, RegisterFinish) {
  const Object* answer = Response("register.finish");
  ASSERT_NE(answer, nullptr);
  Answer(*answer);
  HttpRegistrationClient client(server_.Origin() + "/api/relay");
  auto result = client.FinishRegistration("golden-challenge", Base64Encode(Keys().typed_public_key), "alice",
                                          SignedRelayCommand{.sign_version = kRelaySignVersion},
                                          Base64Encode(Keys().typed_kem_public_key));
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->success);
  EXPECT_EQ(result->account_id, Keys().account_id);
  EXPECT_EQ(result->registration_id, "golden_registration_01");
  EXPECT_EQ(result->llm_token.Reveal(), "brf_llm_golden");
  EXPECT_TRUE(result->kem_bound);
  EXPECT_EQ(result->account_card_status, "stored");
}

TEST_F(AnswerFixtureTest, InboxPoll) {
  const Object* answer = Response("inbox.poll");
  ASSERT_NE(answer, nullptr);
  Answer(*answer);
  HttpRelayClient client(server_.Origin() + "/api/relay");
  client.SetAuthSigner(test::FakeRelaySigner());
  client.SetAudience(std::make_shared<FixedRelayAudience>(Keys().peer_a));
  auto result = client.PollInbox(Keys().account_id, "");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->messages.empty());
  EXPECT_EQ(result->next_cursor, "golden-cursor-2");
  EXPECT_EQ(result->server_time_ms, kNowMs + 123);
}

// The account card (v5) verifies with its bytes; every refused card is refused (account_card_vectors.json holds the
// whole set, with each one's verdict).
TEST(RelaySignVectorsTest, TheCardVerifiesAndTheRefusedCardsAreRefused) {
  const Object* vector = Committed().getObject("account_card");
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

  const std::vector<const Object*> refused = Rows("refused_cards");
  EXPECT_EQ(refused.size(), 5u);
  for (const Object* row : refused) {
    const Object* json = row->getObject("card");
    ASSERT_NE(json, nullptr);
    auto parsed = AccountCardFromJson(*json);
    EXPECT_FALSE(parsed && VerifyAccountCard(*parsed, kNowMs)) << Str(*row, "name");
  }
}

} // namespace
} // namespace pbr
