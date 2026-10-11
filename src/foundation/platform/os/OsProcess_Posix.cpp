#if !defined(_WIN32)

#include "foundation/platform/os/OsProcess.h"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace pbr::os {

OsProcessPipe::~OsProcessPipe() {
  Stop();
}

bool OsProcessPipe::Start(const std::string& command, const std::vector<std::string>& args) {
  Stop();

  int stdin_pipe[2]{};
  int stdout_pipe[2]{};
  // The child writes errno here if exec fails; the write end closes on a successful exec, so the parent reads
  // nothing. Without it a missing command looked like a running server until the first read (PB-056).
  int exec_status_pipe[2]{};
  if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(exec_status_pipe) != 0) {
    return false;
  }
  fcntl(exec_status_pipe[1], F_SETFD, FD_CLOEXEC);

  child_pid_ = fork();
  if (child_pid_ == 0) {
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(exec_status_pipe[0]);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(command.c_str()));
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    execvp(command.c_str(), argv.data());
    const int exec_errno = errno;
    (void)write(exec_status_pipe[1], &exec_errno, sizeof(exec_errno));
    _exit(127);
  }

  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  close(exec_status_pipe[1]);
  int exec_errno = 0;
  const ssize_t status_bytes = read(exec_status_pipe[0], &exec_errno, sizeof(exec_errno));
  close(exec_status_pipe[0]);
  if (status_bytes > 0 || child_pid_ < 0) {
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    if (child_pid_ > 0) {
      waitpid(child_pid_, nullptr, 0);
    }
    child_pid_ = -1;
    return false;
  }

  stdin_write_fd_ = stdin_pipe[1];
  stdout_read_fd_ = stdout_pipe[0];
  active_ = true;
  return true;
}

void OsProcessPipe::Stop() {
  if (stdin_write_fd_ >= 0) {
    close(stdin_write_fd_);
    stdin_write_fd_ = -1;
  }
  if (stdout_read_fd_ >= 0) {
    close(stdout_read_fd_);
    stdout_read_fd_ = -1;
  }
  if (child_pid_ > 0) {
    kill(child_pid_, SIGTERM);
    waitpid(child_pid_, nullptr, 0);
    child_pid_ = -1;
  }
  active_ = false;
}

int OsProcessPipe::Write(const void* data, size_t size) {
  if (!active_ || stdin_write_fd_ < 0) {
    return -1;
  }
  const ssize_t written = write(stdin_write_fd_, data, size);
  return written < 0 ? -1 : static_cast<int>(written);
}

int OsProcessPipe::Read(void* buffer, size_t size) {
  if (!active_ || stdout_read_fd_ < 0) {
    return -1;
  }
  const ssize_t bytes = read(stdout_read_fd_, buffer, size);
  return bytes < 0 ? -1 : static_cast<int>(bytes);
}

int OsProcessPipe::Read(void* buffer, size_t size, const int timeout_ms) {
  if (!active_ || stdout_read_fd_ < 0) {
    return -1;
  }
  pollfd waiting{};
  waiting.fd = stdout_read_fd_;
  waiting.events = POLLIN;
  const int ready = poll(&waiting, 1, timeout_ms);
  if (ready == 0) {
    return kReadTimedOut;
  }
  if (ready < 0) {
    return -1;
  }
  return Read(buffer, size);
}

} // namespace pbr::os

#endif
