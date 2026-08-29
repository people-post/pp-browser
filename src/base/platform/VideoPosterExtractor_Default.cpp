#include "base/platform/VideoPosterExtractor.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "common/PbrCompat.h"
#endif

namespace pbr {
namespace {

Roe<std::vector<uint8_t>> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Error("Failed to read ffmpeg poster output");
  }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

Roe<std::vector<uint8_t>> TryFfmpegPoster(const std::string& video_path, const int max_dimension) {
#if defined(_WIN32)
  (void)video_path;
  (void)max_dimension;
  return Error("ffmpeg poster not available");
#else
  if (video_path.empty() || !std::filesystem::exists(video_path)) {
    return Error("Video path missing");
  }

  std::error_code ec;
  const auto tmp_dir = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return Error("No temp directory for video poster");
  }
  const auto tmp_path =
      tmp_dir / ("pp_video_poster_" + std::to_string(getpid()) + "_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jpg");
  std::filesystem::remove(tmp_path, ec);

  const std::string scale = "scale=min(" + std::to_string(std::max(16, max_dimension)) + "\\,iw):-2";
  const std::string out_path = tmp_path.string();

  const pid_t pid = fork();
  if (pid < 0) {
    return Error("Failed to fork ffmpeg for video poster");
  }
  if (pid == 0) {
    execlp("ffmpeg", "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", "0", "-i", video_path.c_str(),
           "-frames:v", "1", "-vf", scale.c_str(), "-q:v", "5", out_path.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  constexpr int kTimeoutMs = 8000;
  constexpr int kPollMs = 100;
  int status = 0;
  bool finished = false;
  for (int waited = 0; waited < kTimeoutMs; waited += kPollMs) {
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      finished = true;
      break;
    }
    if (r < 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
  }
  if (!finished) {
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    std::filesystem::remove(tmp_path, ec);
    return Error("ffmpeg poster timed out");
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::filesystem::remove(tmp_path, ec);
    return Error("ffmpeg poster failed");
  }

  auto bytes = ReadFileBytes(tmp_path);
  std::filesystem::remove(tmp_path, ec);
  if (!bytes) {
    return bytes.error();
  }
  if (bytes->size() < 2 || (*bytes)[0] != 0xFF || (*bytes)[1] != 0xD8) {
    return Error("ffmpeg poster output is not JPEG");
  }
  return bytes;
#endif
}

} // namespace

Roe<std::vector<uint8_t>> ExtractVideoPosterJpeg(const std::string& video_path, const int max_dimension) {
  if (auto jpeg = TryFfmpegPoster(video_path, max_dimension)) {
    return jpeg;
  }
  return SoftVideoPosterJpeg(max_dimension);
}

} // namespace pbr
