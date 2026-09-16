#include "feature/conversations/AttachmentClientUtil.h"

#include "foundation/crypto/AttachmentContentCipher.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "feature/conversations/ChatBlobRequestUtil.h"
#include "foundation/runtime/AppRuntime.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <thread>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Roe<std::string> RequireRegisteredRelayUserId(IdentityStore& identity) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->registered || loaded->relay_user_id.empty()) {
    return Error("Register on the network before sending attachments");
  }
  return loaded->relay_user_id;
}

Roe<ByteVector> ReadFileBytes(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Error("Could not read attachment file");
  }
  const ByteVector bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (bytes.empty()) {
    return Error("Attachment file is empty");
  }
  if (bytes.size() > kMaxChatAttachmentPlaintextBytes) {
    return Error("Attachment exceeds 4 MiB limit");
  }
  return bytes;
}

std::string FilenameFromPath(const std::string& path) {
  const std::filesystem::path file_path(path);
  if (file_path.has_filename()) {
    return file_path.filename().string();
  }
  return "attachment";
}

std::string MimeFromFilename(const std::string& filename) {
  std::string ext = std::filesystem::path(filename).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (ext == ".png") {
    return "image/png";
  }
  if (ext == ".jpg" || ext == ".jpeg") {
    return "image/jpeg";
  }
  if (ext == ".webp") {
    return "image/webp";
  }
  if (ext == ".gif") {
    return "image/gif";
  }
  if (ext == ".pdf") {
    return "application/pdf";
  }
  if (ext == ".txt") {
    return "text/plain";
  }
  if (ext == ".mp4") {
    return "video/mp4";
  }
  if (ext == ".webm") {
    return "video/webm";
  }
  return "application/octet-stream";
}

void TryPeerPushAsync(const PreparedChatAttachment& prepared, IdentityStore& identity,
                      const ChatAttachmentUploadOptions& options,
                      std::function<void(Roe<void>)> on_done) {
  auto finish = [on_done = std::move(on_done)](Roe<void> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };
  if (!options.peer_client || !options.contacts || !options.thread || options.thread_id.empty()) {
    finish(Error("Peer blob client not configured"));
    return;
  }
  if (options.thread->kind != ThreadKind::Direct || !ThreadChannelIsE2e(options.thread->channel)) {
    finish(Error("Peer blob push requires E2E direct thread"));
    return;
  }
  auto request = BuildChatBlobRequest(*options.thread, *options.contacts, identity, ChatBlobOp::Push,
                                      options.thread_id, prepared.fields.content_hash);
  if (!request) {
    finish(request.error());
    return;
  }
  if (!options.peer_client->IsPeerReachable(request->peer_identity_value)) {
    finish(Error("Peer-direct endpoint not registered"));
    return;
  }
  options.peer_client->PushChatBlobAsync(*request, prepared.ciphertext, std::move(finish));
}

Roe<ChatAttachmentFields> UploadPreparedToRelay(IBlobClient& blob, const std::string& relay_user_id,
                                              const PreparedChatAttachment& prepared, const std::string& source_path) {
  const std::string body(reinterpret_cast<const char*>(prepared.ciphertext.data()), prepared.ciphertext.size());
  auto uploaded =
      UploadRelayBlobBytes(blob, relay_user_id, "application/octet-stream", BlobPurpose::File, body);
  if (!uploaded) {
    return uploaded.error();
  }

  ChatAttachmentFields fields = prepared.fields;
  fields.url = uploaded.value().public_url;
  if (fields.mime.empty()) {
    fields.mime = MimeFromFilename(FilenameFromPath(source_path));
  }
  if (fields.filename.empty()) {
    fields.filename = FilenameFromPath(source_path);
  }
  return fields;
}

} // namespace

Roe<PreparedChatAttachment> PrepareChatAttachmentFromFile(const std::string& path) {
  auto plaintext = ReadFileBytes(path);
  if (!plaintext) {
    return plaintext.error();
  }

  auto content_key = AttachmentContentCipher::GenerateContentKey();
  if (!content_key) {
    return content_key.error();
  }
  auto encrypted = AttachmentContentCipher::Encrypt(*content_key, *plaintext);
  if (!encrypted) {
    return encrypted.error();
  }
  auto content_hash = AttachmentContentHash(*plaintext);
  if (!content_hash) {
    return content_hash.error();
  }

  PreparedChatAttachment prepared;
  prepared.fields.mime = MimeFromFilename(FilenameFromPath(path));
  prepared.fields.filename = FilenameFromPath(path);
  prepared.fields.byte_length = plaintext->size();
  prepared.fields.content_hash = *content_hash;
  prepared.fields.content_key = *content_key;
  prepared.fields.blob_nonce = encrypted->nonce;
  prepared.ciphertext.assign(encrypted->ciphertext.begin(), encrypted->ciphertext.end());
  return prepared;
}

void UploadChatAttachmentFromFileAsync(IBlobClient& blob, IdentityStore& identity, const std::string& path,
                                       ChatAttachmentUploadOptions options,
                                       std::function<void(Roe<ChatAttachmentFields>)> on_done) {
  auto finish = [on_done = std::move(on_done)](Roe<ChatAttachmentFields> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };

  auto relay_user_id = RequireRegisteredRelayUserId(identity);
  if (!relay_user_id) {
    finish(relay_user_id.error());
    return;
  }

  auto prepared = PrepareChatAttachmentFromFile(path);
  if (!prepared) {
    finish(prepared.error());
    return;
  }

  if (options.thread && options.thread->kind == ThreadKind::Direct && ThreadChannelIsE2e(options.thread->channel)) {
    const std::string relay_id = *relay_user_id;
    PreparedChatAttachment prepared_copy = *prepared;
    TryPeerPushAsync(
        prepared_copy, identity, options,
        [&blob, relay_id, prepared_copy = std::move(prepared_copy), path, finish](Roe<void> pushed) mutable {
          auto continue_result = [&blob, relay_id, prepared_copy = std::move(prepared_copy), path,
                                  finish = std::move(finish), pushed = std::move(pushed)]() mutable {
            if (pushed) {
              finish(prepared_copy.fields);
              return;
            }
            finish(UploadPreparedToRelay(blob, relay_id, prepared_copy, path));
          };
          if (AppRuntime::IsRunning()) {
            AppRuntime::PostWorkerNormal(std::move(continue_result));
          } else {
            continue_result();
          }
        });
    return;
  }

  finish(UploadPreparedToRelay(blob, *relay_user_id, *prepared, path));
}

Roe<ChatAttachmentFields> UploadChatAttachmentFromFile(IBlobClient& blob, IdentityStore& identity,
                                                       const std::string& path,
                                                       const ChatAttachmentUploadOptions& options) {
  auto result_promise = std::make_shared<std::promise<Roe<ChatAttachmentFields>>>();
  auto result_future = result_promise->get_future();
  UploadChatAttachmentFromFileAsync(blob, identity, path, options,
                                    [result_promise](Roe<ChatAttachmentFields> value) {
                                      try {
                                        result_promise->set_value(std::move(value));
                                      } catch (const std::future_error&) {
                                      }
                                    });
  constexpr auto kWait = std::chrono::milliseconds(60000);
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return Error("Attachment upload timed out");
  }
  return result_future.get();
}

} // namespace pbr
