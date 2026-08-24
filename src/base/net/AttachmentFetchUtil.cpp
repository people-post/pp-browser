#include "base/net/AttachmentFetchUtil.h"

#include "base/crypto/AttachmentContentCipher.h"
#include "base/net/HttpClient.h"

namespace pbr {

Roe<std::vector<uint8_t>> FetchAndDecryptAttachment(const ChatAttachmentFields& fields) {
  if (fields.url.empty()) {
    return Error("Attachment URL is required");
  }
  const auto response = HttpClient::Get(fields.url);
  if (!response) {
    return response.error();
  }
  const HttpResponse& http = response.value();
  if (http.status_code < 200 || http.status_code >= 300) {
    return Error("Attachment download failed with status " + std::to_string(http.status_code));
  }
  if (http.body.empty()) {
    return Error("Attachment download returned empty body");
  }

  const ByteVector ciphertext(http.body.begin(), http.body.end());
  auto plaintext = AttachmentContentCipher::Decrypt(fields.content_key, fields.blob_nonce, ciphertext,
                                                    fields.content_hash);
  if (!plaintext) {
    return plaintext.error();
  }
  return std::vector<uint8_t>(plaintext->begin(), plaintext->end());
}

} // namespace pbr
