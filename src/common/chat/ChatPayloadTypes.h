#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

enum class ChatContentType { Text, System, Annotation, ContactCard, CryptoTx, Attachment, Unsupported };

struct ChatAnnotationFields {
  std::string text;
  std::string annotation_type;
  std::string target_message_id;
  std::string value;
};

struct ChatContactCardFields {
  std::string contact_id;
  std::string display_name;
  std::string relay_user_id;
  std::string avatar_url;
};

struct ChatCryptoTxFields {
  std::string chain_id;
  std::string asset;
  std::string amount;
  std::string direction;
  std::string tx_hash;
  std::string status;
  std::string to_address;
};

/** E2E attachment metadata — file ciphertext lives on CDN (R004/R007). */
struct ChatAttachmentFields {
  std::string url;
  std::string mime;
  std::string filename;
  uint64_t byte_length = 0;
  std::vector<uint8_t> content_hash;
  std::vector<uint8_t> content_key;
  std::vector<uint8_t> blob_nonce;
};

std::string ChatContentTypeToDb(ChatContentType type);
ChatContentType ChatContentTypeFromDb(const std::string& value);

} // namespace pbr
