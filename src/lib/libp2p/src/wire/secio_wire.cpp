/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/wire/secio_wire.hpp>

namespace libp2p::wire {

outcome::result<Bytes> SecioProposeWire::encode() const {
  Writer w;
  if (rand) {
    w.writeBytesField(1, *rand);
  }
  if (pubkey) {
    w.writeBytesField(2, *pubkey);
  }
  if (exchanges) {
    w.writeStringField(3, *exchanges);
  }
  if (ciphers) {
    w.writeStringField(4, *ciphers);
  }
  if (hashes) {
    w.writeStringField(5, *hashes);
  }
  return w.take();
}

outcome::result<SecioProposeWire> SecioProposeWire::decode(BytesIn bytes) {
  SecioProposeWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(field_1, r.readLengthDelimited());
        out.rand = std::move(field_1);
        break;
      }
      case 2: {
        OUTCOME_TRY(field_2, r.readLengthDelimited());
        out.pubkey = std::move(field_2);
        break;
      }
      case 3: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.exchanges = std::string(value.begin(), value.end());
        break;
      }
      case 4: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.ciphers = std::string(value.begin(), value.end());
        break;
      }
      case 5: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.hashes = std::string(value.begin(), value.end());
        break;
      }
      default: {
        OUTCOME_TRY(r.skipField(tag.type));
        break;
      }
    }
  }
  return out;
}

outcome::result<Bytes> SecioExchangeWire::encode() const {
  Writer w;
  if (epubkey) {
    w.writeBytesField(1, *epubkey);
  }
  if (signature) {
    w.writeBytesField(2, *signature);
  }
  return w.take();
}

outcome::result<SecioExchangeWire> SecioExchangeWire::decode(BytesIn bytes) {
  SecioExchangeWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(field_3, r.readLengthDelimited());
        out.epubkey = std::move(field_3);
        break;
      }
      case 2: {
        OUTCOME_TRY(field_4, r.readLengthDelimited());
        out.signature = std::move(field_4);
        break;
      }
      default: {
        OUTCOME_TRY(r.skipField(tag.type));
        break;
      }
    }
  }
  return out;
}

}  // namespace libp2p::wire
