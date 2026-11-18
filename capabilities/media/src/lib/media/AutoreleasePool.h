#pragma once

namespace pbr {

/**
 * Drains the Objective-C autorelease pool of the scope on Apple platforms; elsewhere does nothing.
 * Long-lived worker threads (plain std::thread, no run loop) have no pool of their own: what the OS
 * frameworks autorelease on them (camera / audio open and close, frame reads) would live until the
 * thread exits. Put one at the top of each task or loop iteration.
 */
class AutoreleasePoolScope {
public:
  AutoreleasePoolScope();
  ~AutoreleasePoolScope();
  AutoreleasePoolScope(const AutoreleasePoolScope&) = delete;
  AutoreleasePoolScope& operator=(const AutoreleasePoolScope&) = delete;

private:
  [[maybe_unused]] void* token_ = nullptr; // only the Apple implementation uses it
};

} // namespace pbr
