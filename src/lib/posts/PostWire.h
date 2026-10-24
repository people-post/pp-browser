#pragma once

#include "lib/posts/PostTypes.h"

#include "common/Serialize.hpp"

#include <sstream>
#include <string>
#include "common/PbrCompat.h"

namespace pbr::post_wire {

/** PP binary wire profile writer for post encodings (u64 BE, LenUtf8, fixed byte arrays). */
class Writer {
public:
  Writer() : ar_(oss_) {}

  void Raw(const std::string& bytes) { oss_.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
  void U8(uint8_t value) { ar_ & value; }
  void U64(uint64_t value) { ar_ & value; }
  void I64(int64_t value) { ar_ & value; }
  void Bool(bool value) { ar_ & value; }
  void Utf8(const std::string& value) {
    WireLenUtf8 wrapped{value};
    ar_ & wrapped;
  }
  void Bytes(const ByteVector& value) {
    WireLenBytes wrapped{value};
    ar_ & wrapped;
  }
  void Lane(const PostLane& lane) { ar_ & lane; }
  void Hash(const PostHash& hash) { ar_ & hash; }
  void Ref(const PostRef& ref) {
    Utf8(ref.author);
    Lane(ref.lane);
    U64(ref.seq);
    Hash(ref.entry_hash);
  }
  ByteVector Take() {
    const std::string data = oss_.str();
    return ByteVector(data.begin(), data.end());
  }

private:
  std::ostringstream oss_;
  OutputArchive ar_;
};

/** The matching reader: values read after a failure are empty; check failed() / AtEnd() once at the end. */
class Reader {
public:
  explicit Reader(const ByteVector& bytes) : iss_(std::string(bytes.begin(), bytes.end())), ar_(iss_) {}

  /** The next bytes are exactly `bytes`. */
  bool Expect(const std::string& bytes) {
    std::string got(bytes.size(), '\0');
    if (!iss_.read(got.data(), static_cast<std::streamsize>(got.size())) || got != bytes) {
      failed_ = true;
    }
    return !failed();
  }
  uint8_t U8() { return Read<uint8_t>(); }
  uint64_t U64() { return Read<uint64_t>(); }
  int64_t I64() { return Read<int64_t>(); }
  bool Bool() { return Read<bool>(); }
  std::string Utf8() {
    WireLenUtf8 wrapped;
    ar_ & wrapped;
    return failed() ? std::string() : wrapped.value;
  }
  ByteVector Bytes() {
    WireLenBytes wrapped;
    ar_ & wrapped;
    return failed() ? ByteVector() : wrapped.value;
  }
  PostLane Lane() { return Read<PostLane>(); }
  PostHash Hash() { return Read<PostHash>(); }
  PostRef Ref() {
    PostRef ref;
    ref.author = Utf8();
    ref.lane = Lane();
    ref.seq = U64();
    ref.entry_hash = Hash();
    return ref;
  }
  bool failed() const { return failed_ || ar_.failed(); }
  /** Everything read, nothing left over. */
  bool AtEnd() const { return !failed() && ar_.exactEnd(); }

private:
  template <typename T>
  T Read() {
    T value{};
    ar_ & value;
    return failed() ? T{} : value;
  }

  std::istringstream iss_;
  InputArchive ar_;
  bool failed_ = false;
};

} // namespace pbr::post_wire
