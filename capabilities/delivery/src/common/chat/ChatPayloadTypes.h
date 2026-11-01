#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** `Deleted`: a local tombstone — the row keeps its id (so sync cannot bring the message back) and no content. */
enum class ChatContentType { Text, System, Annotation, ContactCard, CryptoTx, Attachment, Unsupported, Deleted };

struct ChatAnnotationFields {
  std::string text;
  std::string annotation_type;
  std::string target_message_id;
  std::string value;
};

/** Someone's contact, shared in a chat (WIRE_SCHEMAS.md contact_card sub_version 2, ACCOUNT_CARD.md). */
struct ChatContactCardFields {
  /** The sharer's name for the person: a label, not an identity. */
  std::string display_name;
  /** The person's account card — or key card when the sharer has none — as JSON; checked before *Add*. */
  std::string card_json;
  /** Where the relay delivers to the person, when the sharer knows it (a hint; empty when not). */
  std::string relay_user_id;
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
