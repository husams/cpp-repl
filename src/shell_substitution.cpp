#include "commands.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <optional>
#include <ostream>
#include <string>

#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char **environ;
#endif

namespace example {
namespace {

class Descriptor {
public:
  explicit Descriptor(int fd) : fd_(fd) {}
  ~Descriptor() { close(fd_); }
  Descriptor(const Descriptor &) = delete;
  Descriptor &operator=(const Descriptor &) = delete;
  int get() const { return fd_; }
  void reset() {
    close(fd_);
    fd_ = -1;
  }

private:
  int fd_;
};

std::optional<std::string> capture_shell(const std::string &script,
                                         std::string &error,
                                         std::ostream* destination = nullptr) {
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    error = "could not create shell output pipe: " +
            std::string(std::strerror(errno));
    return std::nullopt;
  }
  Descriptor reader(pipe_fds[0]);
  Descriptor writer(pipe_fds[1]);
  posix_spawn_file_actions_t actions;
  int code = posix_spawn_file_actions_init(&actions);
  if (code != 0) {
    error =
        "could not prepare shell process: " + std::string(std::strerror(code));
    return std::nullopt;
  }
  // Child stdin cannot consume subsequent piped REPL commands. Substitutions
  // capture stdout and retain stderr; direct commands stream both outputs.
  code = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                          O_RDONLY, 0);
  if (code == 0)
    code =
        posix_spawn_file_actions_adddup2(&actions, writer.get(), STDOUT_FILENO);
  if (code == 0 && destination)
    code = posix_spawn_file_actions_adddup2(&actions, writer.get(), STDERR_FILENO);
  if (code == 0)
    code = posix_spawn_file_actions_addclose(&actions, reader.get());
  if (code == 0)
    code = posix_spawn_file_actions_addclose(&actions, writer.get());
  std::array<char *, 4> argv{const_cast<char *>("sh"), const_cast<char *>("-c"),
                             const_cast<char *>(script.c_str()), nullptr};
#ifdef __APPLE__
  char **environment = *_NSGetEnviron();
#else
  char **environment = environ;
#endif
  pid_t child = -1;
  if (code == 0)
    code = posix_spawn(&child, "/bin/sh", &actions, nullptr, argv.data(),
                       environment);
  posix_spawn_file_actions_destroy(&actions);
  if (code != 0) {
    error =
        "could not launch shell process: " + std::string(std::strerror(code));
    return std::nullopt;
  }
  writer.reset();
  std::string output;
  std::array<char, 4096> buffer;
  while (true) {
    const auto count = read(reader.get(), buffer.data(), buffer.size());
    if (count == 0)
      break;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      error =
          "could not read shell output: " + std::string(std::strerror(errno));
      break;
    }
    if (destination) {
      destination->write(buffer.data(), count);
      destination->flush();
    } else {
      output.append(buffer.data(), static_cast<std::size_t>(count));
    }
  }
  reader.reset();
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0)
    error = "could not wait for shell process: " +
            std::string(std::strerror(errno));
  else if (WIFSIGNALED(status))
    error = "shell process terminated by signal " +
            std::to_string(WTERMSIG(status));
  else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    error = "shell process failed with exit status " +
            std::to_string(WEXITSTATUS(status));
  if (!error.empty())
    return std::nullopt;
  if (output.find('\0') != std::string::npos) {
    error = "shell output contains a NUL byte";
    return std::nullopt;
  }
  while (!output.empty() && output.back() == '\n')
    output.pop_back();
  return output;
}

} // namespace

bool execute_shell(const std::string& script, std::ostream& out, std::string& error) {
  error.clear();
  out.flush();
  return capture_shell(script, error, &out).has_value();
}

ParsedCommand expand_shell_substitutions(const ParsedCommand &command) {
  if (!command.error.empty() || command.shell_commands.empty())
    return command;
  std::string expanded;
  std::size_t previous = 0;
  for (const auto &shell : command.shell_commands) {
    std::string error;
    const auto output = capture_shell(shell.value, error);
    if (!output) {
      auto failed = command;
      failed.error = std::move(error);
      failed.error_span = shell.span;
      return failed;
    }
    expanded.append(command.original_text, previous,
                    shell.span.begin - previous);
    expanded += *output;
    previous = shell.span.end;
  }
  expanded.append(command.original_text, previous, std::string::npos);
  // Captured text is parsed as arguments, never executed as more substitutions.
  return parse_command(expanded, false);
}

} // namespace example
