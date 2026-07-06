#pragma once

#include <optional>
#include <string>

namespace pbr {

enum class ChatContentType { Text, System, Annotation, ContactCard, CryptoTx };

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

std::string ChatContentTypeToDb(ChatContentType type);
ChatContentType ChatContentTypeFromDb(const std::string& value);

} // namespace pbr
