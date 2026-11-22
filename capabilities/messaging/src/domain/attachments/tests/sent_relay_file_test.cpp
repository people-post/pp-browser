// B236: before a picture / file / voice message of ours is sent again as itself (a tap on its red mark), whether the
// server copy it points at is known to be gone — then the tap says so in plain words instead of sending a message the
// recipient could not open.
#include "domain/attachments/SentRelayFile.h"

#include "lib/delivery/ChatPayloadCodec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace pbr {
namespace {

constexpr int64_t kSentAt = 1'000'000'000;
constexpr const char kBlobId[] = "0b8c2a52-6f2e-4a52-9d1e-1f2a3b4c5d6e";

std::string Payload(const std::string& url) {
  ChatAttachmentFields fields;
  fields.url = url;
  fields.mime = "image/jpeg";
  fields.filename = "photo.jpg";
  fields.byte_length = 1234;
  fields.content_hash = std::vector<uint8_t>(32, 0x11);
  fields.content_key = std::vector<uint8_t>(32, 0x22);
  fields.blob_nonce = std::vector<uint8_t>(24, 0x33);
  return ChatPayloadCodec::AttachmentFieldsToJson(fields);
}

std::string Link(const std::string& blob_id) {
  return "https://api.pp.example/api/relay/v1/blob/" + blob_id + "/download?t=token";
}

RelayFileListResult Files(const std::vector<std::string>& blob_ids) {
  RelayFileListResult listed;
  for (const std::string& id : blob_ids) {
    listed.files.push_back(RelayFileRecord{.blob_id = id, .status = "complete"});
  }
  return listed;
}

TEST(SentRelayFileTest, OnTheServerStillIsNotGone) {
  const RelayFileListResult ours = Files({"other", kBlobId});
  EXPECT_FALSE(SentRelayFileGone(Payload(Link(kBlobId)), kSentAt, kSentAt + 60'000, &ours));
}

TEST(SentRelayFileTest, DeletedFromOurFilesIsGone) {
  const RelayFileListResult ours = Files({"other"});
  EXPECT_TRUE(SentRelayFileGone(Payload(Link(kBlobId)), kSentAt, kSentAt + 60'000, &ours)) << "Me > Storage freed it";
}

TEST(SentRelayFileTest, PastTheRelaysFileLifetimeIsGone) {
  EXPECT_FALSE(SentRelayFileGone(Payload(Link(kBlobId)), kSentAt, kSentAt + kRelayFileLifetimeMs, nullptr));
  EXPECT_TRUE(SentRelayFileGone(Payload(Link(kBlobId)), kSentAt, kSentAt + kRelayFileLifetimeMs + 1, nullptr));
}

TEST(SentRelayFileTest, WhatCannotBeToldIsNotGone) {
  EXPECT_FALSE(SentRelayFileGone(Payload(Link(kBlobId)), kSentAt, kSentAt + 1, nullptr)) << "no listing (offline)";
  RelayFileListResult full;
  for (size_t i = 0; i < kRelayFileListCap; ++i) {
    full.files.push_back(RelayFileRecord{.blob_id = "f" + std::to_string(i)});
  }
  EXPECT_FALSE(SentRelayFileGone(Payload(Link(kBlobId)), kSentAt, kSentAt + 1, &full))
      << "a full list may leave it out";
  const RelayFileListResult none = Files({});
  EXPECT_FALSE(SentRelayFileGone(Payload(""), kSentAt, kSentAt + kRelayFileLifetimeMs * 2, &none))
      << "a direct push carried it: no server copy to lose";
  EXPECT_FALSE(SentRelayFileGone("{\"not\":\"an attachment\"}", kSentAt, kSentAt + 1, &none));
}

} // namespace
} // namespace pbr
