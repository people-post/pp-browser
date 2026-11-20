#pragma once

#include <map>
#include <string>

namespace pbr {

/** The version header on every call to a relay (release-compatibility O1; SERVICE_ENDPOINTS § Version header). */
inline constexpr char kRelayClientHeaderName[] = "X-PP-Client";

/**
 * The header's value, `<product>/<version> <os>` (RelayClientHeaderValue in lib/net/ClientCompat.h builds it).
 * Process-wide, empty by default (nothing is sent: tests, tools); the app and pp-node set it once at start-up. Every
 * relay client adds it (WithRelayClientHeader): HttpClient's relay callers, the AI stream and the LLM client on the
 * relay's gateway, pp-node's heartbeat. Never sent to a host that is not a relay (a peer's attachment link, a feed,
 * another LLM provider): it names the app's version and OS.
 */
void SetRelayClientHeader(std::string value);
std::string RelayClientHeader();

/** `headers` with the version header added when one is set (a header of that name the caller set stays). */
std::map<std::string, std::string> WithRelayClientHeader(std::map<std::string, std::string> headers = {});

/** Tests: the value for this scope; the previous one comes back after it. */
class ScopedRelayClientHeaderForTest {
public:
  explicit ScopedRelayClientHeaderForTest(std::string value) : previous_(RelayClientHeader()) {
    SetRelayClientHeader(std::move(value));
  }
  ~ScopedRelayClientHeaderForTest() { SetRelayClientHeader(std::move(previous_)); }
  ScopedRelayClientHeaderForTest(const ScopedRelayClientHeaderForTest&) = delete;
  ScopedRelayClientHeaderForTest& operator=(const ScopedRelayClientHeaderForTest&) = delete;

private:
  std::string previous_;
};

} // namespace pbr
