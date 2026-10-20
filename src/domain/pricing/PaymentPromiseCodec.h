#pragma once

#include "common/directory/IAccountSigningAccess.h"
#include "foundation/crypto/CryptoTypes.h"
#include "foundation/data/PaymentPromiseTypes.h"
#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * JSON codec + ML-DSA-65 sign/verify for PaymentPromise receipts (P002).
 * Canonical sign payloads exclude signatures, outcome fields, and local_avoid.
 */
class PaymentPromiseCodec {
public:
  static Roe<std::string> Encode(const PaymentPromise& promise);
  static Roe<PaymentPromise> Decode(const std::string& json);

  /** Deterministic UTF-8 bytes covered by payer/payee promise signatures. */
  static Roe<ByteVector> BuildPromiseSignBytes(const PaymentPromise& promise);
  /** Deterministic UTF-8 bytes covered by outcome signature. */
  static Roe<ByteVector> BuildOutcomeSignBytes(const PaymentPromise& promise);

  static Roe<std::string> SignPromise(const ByteVector& secret_key, const PaymentPromise& promise);
  static Roe<std::string> SignPromise(IAccountSigningAccess& identity, const PaymentPromise& promise);
  static Roe<bool> VerifyPromiseSignature(const ByteVector& public_key, const PaymentPromise& promise,
                                          const std::string& signature_b64);

  static Roe<std::string> SignOutcome(const ByteVector& secret_key, const PaymentPromise& promise);
  static Roe<std::string> SignOutcome(IAccountSigningAccess& identity, const PaymentPromise& promise);
  static Roe<bool> VerifyOutcomeSignature(const ByteVector& public_key, const PaymentPromise& promise);
};

} // namespace pbr
