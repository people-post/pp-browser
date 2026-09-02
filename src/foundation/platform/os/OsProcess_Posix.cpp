#if !defined(_WIN32)

#include "foundation/platform/os/OsProcess.h"

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
  if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
    return false;
  }

  child_pid_ = fork();
  if (child_pid_ == 0) {
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(command.c_str()));
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    execvp(command.c_str(), argv.data());
    _exit(127);
  }

  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
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

} // namespace pbr::os

#endif
