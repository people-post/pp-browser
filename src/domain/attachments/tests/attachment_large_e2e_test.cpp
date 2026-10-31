// A manual end-to-end run of a large attachment against DEV/tools/fake_blob_server.py, to read the peak memory
// of a send, a receive and an open. Skipped unless PP_E2E_BLOB_URL is set; run one phase per process so the
// process high-water mark belongs to that phase alone:
//
//   python3 DEV/tools/fake_blob_server.py --port 8767 &
//   for p in send receive open; do
//     PP_E2E_BLOB_URL=http://127.0.0.1:8767 PP_E2E_DIR=<dir> PP_E2E_PHASE=$p PP_E2E_MB=100 \
//       pp_browser_domain_attachments_test --gtest_filter='LargeAttachmentE2e*'
//   done
//
// The limits are passed in by the test (the shipping default is still 4 MB); the CDN is plain http on loopback,
// which only the test-only flag allows. It uses the real HttpBlobClient, OutgoingAttachmentStore, streamed
// upload / capped file download and the private CAS, i.e. the same calls the app's send / receive / open make.
#include "domain/attachments/AttachmentClient.h"
#include "domain/attachments/AttachmentContent.h"
#include "domain/attachments/AttachmentFetch.h"
#include "domain/attachments/AttachmentMetrics.h"
#include "foundation/crypto/CryptoConstants.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/net/HttpBlobClient.h"
#include "lib/people/IdentityStore.h"
#include "common/Utilities.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>

namespace pbr {
namespace {

std::string Env(const char* name, const std::string& fallback = {}) {
  const char* value = std::getenv(name);
  return value ? value : fallback;
}

ByteVector Dek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0x44 + i);
  }
  return dek;
}

void Report(const char* phase, const uint64_t bytes, const int64_t ms) {
  std::cout << "E2E phase=" << phase << " mb=" << bytes / 1'000'000 << " ms=" << ms
            << " peak_rss_mb=" << PeakResidentBytes() / 1'000'000 << std::endl;
}

TEST(LargeAttachmentE2e, SendReceiveOpenAgainstTheFakeBlobServer) {
  const std::string url = Env("PP_E2E_BLOB_URL");
  if (url.empty()) {
    GTEST_SKIP() << "set PP_E2E_BLOB_URL to run";
  }
  const std::string phase = Env("PP_E2E_PHASE", "send");
  const std::filesystem::path dir = Env("PP_E2E_DIR");
  ASSERT_FALSE(dir.empty());
  std::filesystem::create_directories(dir);
  const uint64_t size = std::stoull(Env("PP_E2E_MB", "100")) * 1'000'000ULL;
  const std::string profile_id = "e2e";
  const std::string source = (dir / "source.bin").string();
  const std::string fields_path = (dir / "fields.json").string();
  const int64_t started = util::NowUnixMs();

  if (phase == "send") {
    if (!std::filesystem::exists(source) || std::filesystem::file_size(source) != size) {
      std::ofstream out(source, std::ios::binary);
      std::string chunk(1 << 20, '\0');
      uint32_t state = 7;
      for (uint64_t left = size; left > 0;) {
        for (size_t i = 0; i < chunk.size(); ++i) {
          state = state * 1664525u + 1013904223u;
          chunk[i] = static_cast<char>(state >> 24);
        }
        const uint64_t n = std::min<uint64_t>(left, chunk.size());
        out.write(chunk.data(), static_cast<std::streamsize>(n));
        left -= n;
      }
    }
    IdentityStore identity(dir.string(), profile_id);
    ASSERT_TRUE(identity.SetDek(Dek()));
    ASSERT_TRUE(identity.LoadOrCreate());
    LocalIdentity local = *identity.Get();
    local.relay_user_id = "relay:e2e";
    local.registered = true;
    ASSERT_TRUE(identity.Update(local));
    HttpBlobClient blob(url);
    blob.SetAuthSigner([](const std::vector<uint8_t>&) { return Roe<std::string>(std::string("sig")); });
    OutgoingAttachmentStore store(dir.string(), profile_id);

    // What the app does: read the picked file, hand it to the staged upload, keep a local copy in the CAS.
    std::ifstream in(source, std::ios::binary);
    std::vector<uint8_t> plaintext(static_cast<size_t>(std::filesystem::file_size(source)));  // as the app reads it
    in.read(reinterpret_cast<char*>(plaintext.data()), static_cast<std::streamsize>(plaintext.size()));
    in.close();
    std::cout << "E2E after-read peak_rss_mb=" << PeakResidentBytes() / 1'000'000 << std::endl;
    ChatAttachmentUploadOptions options;
    options.max_plaintext_bytes = size + 1;  // the test passes the limit; the shipping default stays 4 MB
    options.thread = Thread{.id = "g1", .kind = ThreadKind::Group, .channel = ThreadChannel::E2e};
    options.thread_id = "g1";
    options.outgoing = &store;
    options.dek = Dek();
    int last_percent = -1;
    options.progress = [&](const uint64_t done, const uint64_t total) {
      const int percent = static_cast<int>(done * 100 / total);
      if (percent / 20 != last_percent / 20) {
        std::cout << "E2E upload " << percent << "%" << std::endl;
      }
      last_percent = percent;
      return true;
    };
    Roe<ChatAttachmentFields> result = Error("not called");
    UploadChatAttachmentAsync(blob, identity, plaintext, "clip.mp4", "", options,
                              [&](Roe<ChatAttachmentFields> value) { result = std::move(value); });
    ASSERT_TRUE(result) << result.error().message;
    std::cout << "E2E after-upload peak_rss_mb=" << PeakResidentBytes() / 1'000'000 << std::endl;
    ASSERT_FALSE(result->url.empty());
    ASSERT_TRUE(CopyAttachmentPlaintext(dir.string(), profile_id, "g1", *result, plaintext, Dek(), size));
    std::ofstream(fields_path) << ChatPayloadCodec::AttachmentFieldsToJson(*result);
    Report("send", size, util::NowUnixMs() - started);
  } else if (phase == "receive") {
    std::ifstream in(fields_path);
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto fields = ChatPayloadCodec::DecodeAttachmentJson(json);
    ASSERT_TRUE(fields);
    const std::filesystem::path receiver = dir / "receiver";
    std::filesystem::create_directories(receiver);
    AttachmentFetchContext context;
    context.thread_id = "g1";
    context.profile_data_dir = receiver.string();
    context.allow_local_cdn_for_test = true;
    int last_percent = -1;
    context.progress = [&](const uint64_t done, const uint64_t total) {
      const int percent = total ? static_cast<int>(done * 100 / total) : 0;
      if (percent / 20 != last_percent / 20) {
        std::cout << "E2E download " << percent << "%" << std::endl;
      }
      last_percent = percent;
      return true;
    };
    // What the workflow does after a fetch: decrypt, then store in the private CAS.
    auto plaintext = FetchAndDecryptAttachment(*fields, context);
    ASSERT_TRUE(plaintext) << plaintext.error().message;
    ASSERT_EQ(plaintext->size(), size);
    const ByteVector bytes = std::move(*plaintext);
    ASSERT_TRUE(SaveAttachmentPlaintext(receiver.string(), profile_id, fields->content_hash, fields->mime, bytes,
                                        fields->filename, Dek()));
    ASSERT_TRUE(StoreAttachmentViewBytes(receiver.string(), "g1", fields->content_hash, fields->mime, fields->filename, bytes));
    Report("receive", size, util::NowUnixMs() - started);
  } else if (phase == "open") {
    std::ifstream in(fields_path);
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto fields = ChatPayloadCodec::DecodeAttachmentJson(json);
    ASSERT_TRUE(fields);
    const std::filesystem::path receiver = dir / "receiver";
    // What opening does after a restart or lock (the views are wiped): decrypt the stored copy into a view file.
    std::filesystem::remove_all(AttachmentViewRoot(receiver.string(), "g1"));
    auto view = EnsureAttachmentView(receiver.string(), profile_id, "g1", fields->content_hash, fields->mime,
                                     fields->filename, Dek());
    ASSERT_TRUE(view) << view.error().message;
    ASSERT_EQ(std::filesystem::file_size(*view), size);
    Report("open", size, util::NowUnixMs() - started);
  } else {
    FAIL() << "PP_E2E_PHASE must be send, receive or open";
  }
}

} // namespace
} // namespace pbr
