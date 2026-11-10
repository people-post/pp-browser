#pragma once

#include "foundation/platform/NativeFileDialog.h"

#include <atomic>
#include <functional>
#include <string>
#include <utility>

namespace pbr {

/**
 * Holds a picked or captured file's path and releases it (ReleasePickedFile) once, when Release() is called or the
 * last holder lets go, whichever comes first. A flow that may end in several ways (done, failed, cancelled, a PIN
 * prompt that never answers) keeps one of these in a std::shared_ptr instead of releasing by hand on each path; a
 * later page can be handed the same shared_ptr. Release is safe from any thread and idempotent.
 */
class PickedFileLease {
public:
  using Releaser = std::function<void(const std::string& path)>;

  explicit PickedFileLease(std::string path, Releaser release = [](const std::string& p) { ReleasePickedFile(p); })
      : path_(std::move(path)), release_(std::move(release)) {}
  ~PickedFileLease() { Release(); }
  PickedFileLease(const PickedFileLease&) = delete;
  PickedFileLease& operator=(const PickedFileLease&) = delete;

  const std::string& path() const { return path_; }

  void Release() {
    if (!released_.exchange(true) && release_ && !path_.empty()) {
      release_(path_);
    }
  }

private:
  std::string path_;
  Releaser release_;
  std::atomic<bool> released_{false};
};

} // namespace pbr
