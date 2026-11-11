#pragma once

namespace pbr {

/**
 * Whether outgoing relay POST/PUT requests carry `x-amz-content-sha256`. Process-wide, off by default; the app
 * sets it once at startup from `relay.content_sha256`. HttpClient and the direct-curl AI clients read it.
 */
void SetSendContentSha256(bool enabled);
bool SendContentSha256();

/** Tests: the switch for this scope; the previous value comes back after it. */
class ScopedSendContentSha256ForTest {
public:
  explicit ScopedSendContentSha256ForTest(bool enabled) : previous_(SendContentSha256()) {
    SetSendContentSha256(enabled);
  }
  ~ScopedSendContentSha256ForTest() { SetSendContentSha256(previous_); }
  ScopedSendContentSha256ForTest(const ScopedSendContentSha256ForTest&) = delete;
  ScopedSendContentSha256ForTest& operator=(const ScopedSendContentSha256ForTest&) = delete;

private:
  bool previous_;
};

} // namespace pbr
