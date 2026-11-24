#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "foundation/platform/os/OsRestart.h"

#include <string>
#include <vector>

namespace pbr::os {

namespace {

std::wstring ToWide(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring wide(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
  return wide;
}

} // namespace

bool RestartSupported() {
  return true;
}

// The command line is the OS's own text for this process (GetCommandLineW), so quoting is preserved exactly; the
// environment and the working directory are inherited. The caller ends this process right after.
bool RestartProcess(const RestartPlan& plan, std::string& error) {
  if (!plan.Ready()) {
    error = "no restart plan";
    return false;
  }
  const std::wstring exe = ToWide(plan.exe);
  const wchar_t* original = GetCommandLineW();
  if (exe.empty() || original == nullptr) {
    error = "no command line";
    return false;
  }
  std::wstring command_line(original);
  command_line.push_back(L'\0'); // CreateProcessW may write into the buffer
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  if (!CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                      &info)) {
    error = "CreateProcessW failed: " + std::to_string(static_cast<unsigned long>(GetLastError()));
    return false;
  }
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  return true;
}

} // namespace pbr::os
