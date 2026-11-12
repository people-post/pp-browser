#pragma once

#include "lib/storage/SqliteThreadStore.h"

#include <memory>
#include <string>

namespace pbr::test {

/** A data key for a test profile. */
inline ByteVector TestStoreDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0x50 + i);
  }
  return dek;
}

/** A thread store as the app has it after unlock: given the profile's data key, so its chat store is open. */
inline std::unique_ptr<SqliteThreadStore> MakeUnlockedThreadStore(const std::string& data_dir) {
  auto store = std::make_unique<SqliteThreadStore>(data_dir);
  (void)store->SetDek(TestStoreDek());
  return store;
}

} // namespace pbr::test
