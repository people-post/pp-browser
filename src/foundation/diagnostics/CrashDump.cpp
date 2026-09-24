#include "foundation/diagnostics/CrashDump.h"

#include "foundation/diagnostics/CrashBreadcrumbs.h"
#include "foundation/runtime/AppVersion.h"
#include "foundation/runtime/ProductBranding.h"
#include "common/PbrCompat.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
// Android Bionic has no execinfo/backtrace (still defines __linux__).
#if (defined(__APPLE__) || defined(__linux__)) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <execinfo.h>
#define PP_BROWSER_HAS_EXECINFO_BACKTRACE 1
#endif
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace pbr {

namespace {

constexpr std::size_t kPathBytes = 1024;
constexpr std::size_t kBreadcrumbDumpBytes = 24 * 1024;

char g_dump_path[kPathBytes] = {};
// Main image load address ("%p"), resolved at Install — lets tools map ASLR frames to the binary.
char g_image_base[32] = {};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_writing{false};
std::terminate_handler g_prev_terminate = nullptr;

#if !defined(_WIN32)
using SignalHandlerFn = void (*)(int);
SignalHandlerFn g_prev_segv = SIG_DFL;
SignalHandlerFn g_prev_abrt = SIG_DFL;
SignalHandlerFn g_prev_fpe = SIG_DFL;
SignalHandlerFn g_prev_ill = SIG_DFL;
SignalHandlerFn g_prev_bus = SIG_DFL;
#endif

void EnsureDiagnosticsDir(const std::string& data_dir) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(data_dir) / "diagnostics", ec);
}

/** Compile-time OS label for dumps (signal-safe constant). */
constexpr const char* CrashOsLabel() {
#if defined(__ANDROID__)
  return "android";
#elif defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
  return "ios";
#else
  return "macos";
#endif
#else
  return "linux";
#endif
}

#if !defined(_WIN32)
void WriteAll(int fd, const char* data, std::size_t len) {
  while (len > 0) {
    const ssize_t n = ::write(fd, data, len);
    if (n <= 0) {
      return;
    }
    data += static_cast<std::size_t>(n);
    len -= static_cast<std::size_t>(n);
  }
}

void WriteCString(int fd, const char* s) {
  if (s == nullptr) {
    return;
  }
  WriteAll(fd, s, std::strlen(s));
}

void WriteSignalDump(int signo) {
  if (g_dump_path[0] == '\0') {
    return;
  }
  if (g_writing.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  const int fd = ::open(g_dump_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return;
  }

  WriteCString(fd, "pp-browser crash dump\n");
  WriteCString(fd, "product=");
  WriteCString(fd, kProductSlug);
  WriteCString(fd, "\nversion=");
  WriteCString(fd, AppVersionString());
  WriteCString(fd, "\nos=");
  WriteCString(fd, CrashOsLabel());
  WriteCString(fd, "\nreason=signal ");

  char num[32];
  std::snprintf(num, sizeof(num), "%d\n", signo);
  WriteCString(fd, num);

#if defined(PP_BROWSER_HAS_EXECINFO_BACKTRACE)
  if (g_image_base[0] != '\0') {
    WriteCString(fd, "image_base=");
    WriteCString(fd, g_image_base);
    WriteCString(fd, "\n");
  }
  void* frames[64];
  const int nframes = ::backtrace(frames, 64);
  WriteCString(fd, "backtrace_frames=");
  std::snprintf(num, sizeof(num), "%d\n", nframes);
  WriteCString(fd, num);
  // Print raw addresses only — backtrace_symbols() is not async-signal-safe.
  for (int i = 0; i < nframes; ++i) {
    std::snprintf(num, sizeof(num), "%p\n", frames[i]);
    WriteCString(fd, num);
  }
#endif

  WriteCString(fd, "--- breadcrumbs ---\n");
  char crumbs[kBreadcrumbDumpBytes];
  CrashBreadcrumbs::CopySignalSafe(crumbs, sizeof(crumbs));
  WriteCString(fd, crumbs);

  ::close(fd);
}

void OnFatalSignal(int signo) {
  WriteSignalDump(signo);
  std::_Exit(128 + (signo & 0x7f));
}
#endif

#if defined(_WIN32)
LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* /*info*/) {
  if (g_dump_path[0] == '\0') {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (g_writing.exchange(true, std::memory_order_acq_rel)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  // Best-effort: FILE* is not ideal in a crash filter, but good enough for v0.
  FILE* f = std::fopen(g_dump_path, "w");
  if (f == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  std::fprintf(f, "pp-browser crash dump\nproduct=%s\nversion=%s\nos=%s\nreason=unhandled_exception\n",
               kProductSlug, AppVersionString(), CrashOsLabel());
  std::fprintf(f, "--- breadcrumbs ---\n");
  char crumbs[kBreadcrumbDumpBytes];
  CrashBreadcrumbs::CopySignalSafe(crumbs, sizeof(crumbs));
  std::fputs(crumbs, f);
  std::fclose(f);
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void OnTerminate() {
  const std::string path = g_dump_path;
  std::string reason = "std::terminate";
  try {
    if (auto ep = std::current_exception()) {
      try {
        std::rethrow_exception(ep);
      } catch (const std::exception& ex) {
        reason = std::string("uncaught: ") + ex.what();
      } catch (...) {
        reason = "uncaught: non-std exception";
      }
    }
  } catch (...) {
    reason = "std::terminate (exception inspect failed)";
  }

  if (!path.empty()) {
    const auto parent = std::filesystem::path(path).parent_path().parent_path();
    CrashDump::WritePending(parent.string(), reason);
  }

  if (g_prev_terminate) {
    g_prev_terminate();
  }
  std::_Exit(1);
}

} // namespace

std::string CrashDump::PendingPath(const std::string& data_dir) {
  return (std::filesystem::path(data_dir) / "diagnostics" / "crash_pending.txt").string();
}

void CrashDump::Install(const std::string& data_dir) {
  if (data_dir.empty()) {
    return;
  }
  if (g_installed.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  EnsureDiagnosticsDir(data_dir);
  const std::string path = PendingPath(data_dir);
  std::snprintf(g_dump_path, sizeof(g_dump_path), "%s", path.c_str());

  CrashBreadcrumbs::InstallLoggerHandler();
  CrashBreadcrumbs::Append(std::string("CrashDump installed path=") + path);

#if defined(PP_BROWSER_HAS_EXECINFO_BACKTRACE)
  Dl_info info{};
  if (::dladdr(reinterpret_cast<void*>(&CrashDump::Install), &info) != 0 && info.dli_fbase != nullptr) {
    std::snprintf(g_image_base, sizeof(g_image_base), "%p", info.dli_fbase);
  }
#endif

  g_prev_terminate = std::set_terminate(OnTerminate);

#if !defined(_WIN32)
  g_prev_segv = ::signal(SIGSEGV, OnFatalSignal);
  g_prev_abrt = ::signal(SIGABRT, OnFatalSignal);
  g_prev_fpe = ::signal(SIGFPE, OnFatalSignal);
  g_prev_ill = ::signal(SIGILL, OnFatalSignal);
#if defined(SIGBUS)
  g_prev_bus = ::signal(SIGBUS, OnFatalSignal);
#endif
  (void)g_prev_segv;
  (void)g_prev_abrt;
  (void)g_prev_fpe;
  (void)g_prev_ill;
  (void)g_prev_bus;
#else
  SetUnhandledExceptionFilter(OnUnhandledException);
#endif
}

bool CrashDump::HasPending(const std::string& data_dir) {
  std::error_code ec;
  return std::filesystem::is_regular_file(PendingPath(data_dir), ec);
}

std::string CrashDump::ReadPending(const std::string& data_dir) {
  const std::string path = PendingPath(data_dir);
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void CrashDump::ClearPending(const std::string& data_dir) {
  std::error_code ec;
  std::filesystem::remove(PendingPath(data_dir), ec);
}

void CrashDump::WritePending(const std::string& data_dir, const std::string& reason) {
  if (data_dir.empty()) {
    return;
  }
  EnsureDiagnosticsDir(data_dir);
  const std::string path = PendingPath(data_dir);

  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return;
  }
  out << "pp-browser crash dump\n";
  out << "product=" << kProductSlug << "\n";
  out << "version=" << AppVersionString() << "\n";
  out << "os=" << CrashOsLabel() << "\n";
  out << "reason=" << reason << "\n";
  out << "--- breadcrumbs ---\n";
  for (const std::string& line : CrashBreadcrumbs::Snapshot()) {
    out << line << "\n";
  }
}

} // namespace pbr
