#include "common/BoundedRead.h"

namespace pbr {

namespace {

template <class Buffer>
Roe<Buffer> ReadUpTo(std::istream& in, const size_t max_bytes) {
  Buffer out;
  char chunk[64 * 1024];
  while (in) {
    in.read(chunk, sizeof(chunk));
    const auto got = static_cast<size_t>(in.gcount());
    if (got == 0) {
      break;
    }
    if (out.size() + got > max_bytes) {
      return Error("file larger than " + std::to_string(max_bytes) + " bytes");
    }
    out.insert(out.end(), chunk, chunk + got);
  }
  if (in.bad()) {
    return Error("file read failed");
  }
  return out;
}

} // namespace

Roe<std::string> ReadBounded(std::istream& in, const size_t max_bytes) { return ReadUpTo<std::string>(in, max_bytes); }

Roe<std::vector<uint8_t>> ReadBoundedBytes(std::istream& in, const size_t max_bytes) {
  return ReadUpTo<std::vector<uint8_t>>(in, max_bytes);
}

} // namespace pbr
