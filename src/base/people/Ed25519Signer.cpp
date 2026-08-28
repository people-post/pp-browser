#include "base/people/Ed25519Signer.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string EncodeBase64(const unsigned char* data, size_t len) {
  static const char* kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    const unsigned int b0 = data[i];
    const unsigned int b1 = (i + 1 < len) ? data[i + 1] : 0;
    const unsigned int b2 = (i + 2 < len) ? data[i + 2] : 0;
    out.push_back(kChars[b0 >> 2]);
    out.push_back(kChars[((b0 & 0x03) << 4) | (b1 >> 4)]);
    out.push_back((i + 1 < len) ? kChars[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=');
    out.push_back((i + 2 < len) ? kChars[b2 & 0x3f] : '=');
  }
  return out;
}

int DecodeBase64Char(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

} // namespace

Roe<Ed25519KeyPair> Ed25519Signer::GenerateKeyPair() {
  EVP_PKEY* pkey = nullptr;
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
  if (!ctx) {
    return Error("Failed to create Ed25519 context");
  }

  if (EVP_PKEY_keygen_init(ctx) <= 0) {
    EVP_PKEY_CTX_free(ctx);
    return Error("Failed to init Ed25519 keygen");
  }

  if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
    EVP_PKEY_CTX_free(ctx);
    return Error("Failed to generate Ed25519 keypair");
  }
  EVP_PKEY_CTX_free(ctx);

  size_t pub_len = 0;
  size_t priv_len = 0;
  if (EVP_PKEY_get_raw_public_key(pkey, nullptr, &pub_len) <= 0 ||
      EVP_PKEY_get_raw_private_key(pkey, nullptr, &priv_len) <= 0) {
    EVP_PKEY_free(pkey);
    return Error("Failed to query Ed25519 key lengths");
  }

  Ed25519KeyPair pair;
  pair.public_key.resize(pub_len);
  pair.private_key.resize(priv_len);
  if (EVP_PKEY_get_raw_public_key(pkey, pair.public_key.data(), &pub_len) <= 0 ||
      EVP_PKEY_get_raw_private_key(pkey, pair.private_key.data(), &priv_len) <= 0) {
    EVP_PKEY_free(pkey);
    return Error("Failed to export Ed25519 keys");
  }

  EVP_PKEY_free(pkey);
  return pair;
}

Roe<std::string> Ed25519Signer::Sign(const std::string& message, const std::vector<uint8_t>& private_key) {
  EVP_PKEY* pkey =
      EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, private_key.data(), private_key.size());
  if (!pkey) {
    return Error("Failed to load Ed25519 private key");
  }

  EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
  if (!md_ctx) {
    EVP_PKEY_free(pkey);
    return Error("Failed to create digest context");
  }

  if (EVP_DigestSignInit(md_ctx, nullptr, nullptr, nullptr, pkey) <= 0) {
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return Error("Failed to init Ed25519 sign");
  }

  size_t sig_len = 0;
  if (EVP_DigestSign(md_ctx, nullptr, &sig_len,
                     reinterpret_cast<const unsigned char*>(message.data()), message.size()) <= 0) {
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return Error("Failed to size Ed25519 signature");
  }

  std::vector<unsigned char> signature(sig_len);
  if (EVP_DigestSign(md_ctx, signature.data(), &sig_len,
                     reinterpret_cast<const unsigned char*>(message.data()), message.size()) <= 0) {
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return Error("Failed to sign message");
  }

  EVP_MD_CTX_free(md_ctx);
  EVP_PKEY_free(pkey);
  signature.resize(sig_len);
  return EncodeBase64(signature.data(), signature.size());
}

Roe<bool> Ed25519Signer::Verify(const std::string& message, const std::string& signature_b64,
                                const std::vector<uint8_t>& public_key) {
  auto signature = FromBase64(signature_b64);
  if (!signature) {
    return signature.error();
  }

  EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, public_key.data(), public_key.size());
  if (!pkey) {
    return Error("Failed to load Ed25519 public key");
  }

  EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
  if (!md_ctx) {
    EVP_PKEY_free(pkey);
    return Error("Failed to create digest context");
  }

  if (EVP_DigestVerifyInit(md_ctx, nullptr, nullptr, nullptr, pkey) <= 0) {
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return Error("Failed to init Ed25519 verify");
  }

  const int ok = EVP_DigestVerify(md_ctx, signature->data(), signature->size(),
                                reinterpret_cast<const unsigned char*>(message.data()), message.size());
  EVP_MD_CTX_free(md_ctx);
  EVP_PKEY_free(pkey);
  if (ok < 0) {
    return Error("Failed to verify Ed25519 signature");
  }
  return ok == 1;
}

std::string Ed25519Signer::ToBase64(const std::vector<uint8_t>& data) {
  return EncodeBase64(data.data(), data.size());
}

Roe<std::vector<uint8_t>> Ed25519Signer::FromBase64(const std::string& encoded) {
  std::vector<uint8_t> out;
  out.reserve(encoded.size() * 3 / 4);

  int val = 0;
  int valb = -8;
  for (char c : encoded) {
    if (c == '=') {
      break;
    }
    const int d = DecodeBase64Char(c);
    if (d == -1) {
      continue;
    }
    val = (val << 6) + d;
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}

} // namespace pbr
