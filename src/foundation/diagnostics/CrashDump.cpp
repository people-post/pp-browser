#include "foundation/diagnostics/CrashDump.h"

#include "foundation/diagnostics/CrashBreadcrumbs.h"
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
#elif defined(__ANDROID__)
// Android: unwind with the C++ ABI unwinder and name each frame with dladdr.
#include <dlfcn.h>
#include <unwind.h>
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
#if defined(PP_BROWSER_HAS_EXECINFO_BACKTRACE)
// Main image load address ("%p"), resolved at Install — lets tools map ASLR frames to the binary.
char g_image_base[32] = {};
/** The app's slug and version (CrashIdentity), set once by Install; read by the handlers. */
char g_product[64] = "app";
char g_version[64] = "0";
#endif
std::atomic<bool> g_installed{false};
std::atomic<bool> g_writing{false};
std::terminate_handler g_prev_terminate = nullptr;

#if !defined(_WIN32)
/** The fatal signals we hook, and what was installed before us (Android: debuggerd's handler). */
constexpr int kHookedSignals[] = {
    SIGSEGV, SIGABRT, SIGFPE, SIGILL,
#if defined(SIGBUS)
    SIGBUS,
#endif
};
constexpr std::size_t kHookedSignalCount = sizeof(kHookedSignals) / sizeof(kHookedSignals[0]);
struct sigaction g_prev_actions[kHookedSignalCount];
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
  WriteCString(fd, g_product);
  WriteCString(fd, "\nversion=");
  WriteCString(fd, g_version);
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
#endif
  // If the dump ends on this line, unwinding itself died (the system handler then has the crash).
  WriteCString(fd, "backtrace=unwinding\n");
  CrashDump::WriteNativeBacktrace(fd);

  WriteCString(fd, "--- breadcrumbs ---\n");
  char crumbs[kBreadcrumbDumpBytes];
  CrashBreadcrumbs::CopySignalSafe(crumbs, sizeof(crumbs));
  WriteCString(fd, crumbs);

  ::close(fd);
}

/** Put back what was installed before Install, for every hooked signal (sigaction: async-signal-safe). */
void RestorePreviousHandlers() {
  for (std::size_t i = 0; i < kHookedSignalCount; ++i) {
    struct sigaction prev = g_prev_actions[i];
    if (!(prev.sa_flags & SA_SIGINFO) && prev.sa_handler == SIG_IGN) {
      prev.sa_handler = SIG_DFL;  // an ignored fault would re-fault forever
    }
    ::sigaction(kHookedSignals[i], &prev, nullptr);
  }
}

/**
 * Write our dump, then hand the signal back to whoever had it before us — Android's debuggerd
 * (tombstone, `logcat -b crash`), macOS ReportCrash, or the default action — instead of exiting:
 * exiting here swallowed the system's crash report (B126: an fdsan abort left 87 bytes and no
 * tombstone). The previous handlers go back first, so a fault while writing (the unwinder) reaches
 * the system handler too instead of cutting the dump off silently. This signal is blocked while
 * we run: the raise is delivered as we return; a fault re-executes and traps into it anyway.
 */
void OnFatalSignal(int signo) {
  RestorePreviousHandlers();
  WriteSignalDump(signo);
  ::raise(signo);
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
  std::fprintf(f, "pp-browser crash dump\nproduct=%s\nversion=%s\nos=%s\nreason=unhandled_exception\n", g_product,
               g_version, CrashOsLabel());
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

#if defined(__ANDROID__)
namespace {

constexpr int kMaxNativeFrames = 64;

struct UnwindState {
  uintptr_t pcs[kMaxNativeFrames];
  int count;
};

_Unwind_Reason_Code CollectFrame(_Unwind_Context* ctx, void* arg) {
  auto* state = static_cast<UnwindState*>(arg);
  if (state->count >= kMaxNativeFrames) {
    return _URC_END_OF_STACK;
  }
  const uintptr_t pc = _Unwind_GetIP(ctx);
  if (pc != 0) {
    state->pcs[state->count++] = pc;
  }
  return _URC_NO_REASON;
}

} // namespace
#endif

void CrashDump::WriteNativeBacktrace(int fd) {
#if defined(PP_BROWSER_HAS_EXECINFO_BACKTRACE)
  char num[32];
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
#elif defined(__ANDROID__)
  // _Unwind_Backtrace and dladdr are not strictly async-signal-safe; a crash dump that
  // occasionally fails is better than none (the handler hands the signal to the system next).
  UnwindState state{};
  _Unwind_Backtrace(CollectFrame, &state);
  char line[160];
  WriteCString(fd, "backtrace_frames=");
  std::snprintf(line, sizeof(line), "%d\n", state.count);
  WriteCString(fd, line);
  // Offline: llvm-symbolizer --obj=<unstripped libmain.so> <off> (or ndk-stack).
  for (int i = 0; i < state.count; ++i) {
    Dl_info info{};
    const bool known = ::dladdr(reinterpret_cast<void*>(state.pcs[i]), &info) != 0;
    const auto base = known ? reinterpret_cast<uintptr_t>(info.dli_fbase) : uintptr_t{0};
    std::snprintf(line, sizeof(line), "frame %d pc=0x%lx base=0x%lx off=0x%lx module=", i,
                  static_cast<unsigned long>(state.pcs[i]), static_cast<unsigned long>(base),
                  static_cast<unsigned long>(known ? state.pcs[i] - base : state.pcs[i]));
    WriteCString(fd, line);
    WriteCString(fd, known && info.dli_fname != nullptr ? info.dli_fname : "?");
    WriteCString(fd, " sym=");
    WriteCString(fd, known && info.dli_sname != nullptr ? info.dli_sname : "?");
    WriteCString(fd, "\n");
  }
#else
  (void)fd;
#endif
}

const char* CrashDump::OsLabel() {
  return CrashOsLabel();
}

std::string CrashDump::PendingPath(const std::string& data_dir) {
  return (std::filesystem::path(data_dir) / "diagnostics" / "crash_pending.txt").string();
}

void CrashDump::Install(const std::string& data_dir, const CrashIdentity& identity) {
  if (data_dir.empty()) {
    return;
  }
  if (g_installed.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  std::snprintf(g_product, sizeof(g_product), "%s", identity.product.c_str());
  std::snprintf(g_version, sizeof(g_version), "%s", identity.version.c_str());

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
  // sigaction, not signal(): the previous action is kept whole (debuggerd's is SA_SIGINFO) so the
  // handler can hand the signal back to it. SA_ONSTACK: Android threads have an alternate signal
  // stack, so a stack overflow can still be reported.
  struct sigaction action {};
  action.sa_handler = OnFatalSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_ONSTACK;
  for (std::size_t i = 0; i < kHookedSignalCount; ++i) {
    ::sigaction(kHookedSignals[i], &action, &g_prev_actions[i]);
  }
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
  out << "product=" << g_product << "\n";
  out << "version=" << g_version << "\n";
  out << "os=" << CrashOsLabel() << "\n";
  out << "reason=" << reason << "\n";
  out << "--- breadcrumbs ---\n";
  for (const std::string& line : CrashBreadcrumbs::Snapshot()) {
    out << line << "\n";
  }
}

} // namespace pbr
