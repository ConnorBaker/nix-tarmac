// watchdog_test WATCHDOG: the contract of tests/watchdog.cpp, checked on
// the binary meson runs every test under
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

const char *self;
const char *watchdog;

void sleep_ms(long ms) {
  timespec left{.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1'000'000};
  while (nanosleep(&left, &left) < 0 && errno == EINTR) {
  }
}

// The test the watchdog runs: `self --child OP [ARG [READY_FD]]`.
auto child(char **argv) -> int {
  const std::string op = argv[2];
  if (op == "exit") {
    return atoi(argv[3]);
  }
  if (op == "abort") {
    abort();
  }
  if (op == "sleep") {
    if (argv[4] != nullptr) {
      const int ready = atoi(argv[4]);
      const ssize_t wrote = write(ready, "r", 1);
      assert(wrote == 1);
      close(ready);
    }
    sleep_ms(atol(argv[3]));
    return 0;
  }
  if (op == "fork") {
    // a grandchild that keeps READY_FD open for as long as it lives
    const int ready = atoi(argv[4]);
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      const ssize_t wrote = write(ready, "r", 1);
      assert(wrote == 1);
      sleep_ms(atol(argv[3]));
      _exit(0);
    }
    close(ready);
    sleep_ms(atol(argv[3]));
    return 0;
  }
  return 99;
}

[[noreturn]] void exec_watchdog(const char *budget,
                                const std::vector<std::string> &args) {
  const rlimit no_core{0, 0};
  setrlimit(RLIMIT_CORE, &no_core);
  std::vector<std::string> owned = args;
  std::vector<char *> argv{const_cast<char *>(watchdog),
                           const_cast<char *>(budget), const_cast<char *>(self),
                           const_cast<char *>("--child")};
  for (auto &arg : owned) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  execv(watchdog, argv.data());
  _exit(127);
}

auto wait_for(pid_t pid) -> int {
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    assert(errno == EINTR);
  }
  return status;
}

// Start `watchdog BUDGET self --child ARGS...` in a process group of its
// own, as meson does, so a timeout kills that case and not this test.
auto start(const char *budget, const std::vector<std::string> &args) -> pid_t {
  const pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    setpgid(0, 0);
    exec_watchdog(budget, args);
  }
  setpgid(pid, pid);
  return pid;
}

constexpr int kWatchdogKilled = 42;

// Start the watchdog in a group led by another process, as meson
// --interactive leaves it; that process exits kWatchdogKilled once the
// watchdog has died of SIGKILL, unless it was killed itself.
auto start_led(const char *budget, const std::vector<std::string> &args)
    -> pid_t {
  const pid_t leader = fork();
  assert(leader >= 0);
  if (leader == 0) {
    setpgid(0, 0);
    const pid_t pid = fork();
    if (pid < 0) {
      _exit(1);
    }
    if (pid == 0) {
      exec_watchdog(budget, args);
    }
    const int status = wait_for(pid);
    _exit(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL ? kWatchdogKilled
                                                             : 1);
  }
  setpgid(leader, leader);
  return leader;
}

} // namespace

int main(int argc, char **argv) {
  if (argc > 2 && strcmp(argv[1], "--child") == 0) {
    return child(argv);
  }
  assert(argc == 2);
  self = argv[0];
  watchdog = argv[1];

  // a test that returns in time ends the way it ended
  {
    const int status = wait_for(start("60", {"exit", "3"}));
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 3);
  }
  {
    const int status = wait_for(start("60", {"abort"}));
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
  }

  // a test that has not returned when its budget runs out is killed,
  // watchdog and all
  {
    const int status = wait_for(start("1", {"sleep", "20000"}));
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  }

  // and so is every process the test started: one left behind would keep
  // meson waiting on the test's output, holding the build as a hang would
  {
    int alive[2];
    const int piped = pipe(alive);
    assert(piped == 0);
    const pid_t pid = start("1", {"fork", "60000", std::to_string(alive[1])});
    close(alive[1]);
    char byte = 0;
    const ssize_t got = read(alive[0], &byte, 1);
    assert(got == 1);
    const int status = wait_for(pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    // the pipe reads end-of-file once no process holds its write end
    pollfd hangup{.fd = alive[0], .events = POLLIN, .revents = 0};
    const int ready = poll(&hangup, 1, 30'000);
    assert(ready == 1 && "a grandchild outlived the watchdog's kill");
    const ssize_t eof = read(alive[0], &byte, 1);
    assert(eof == 0);
    close(alive[0]);
  }

  // a watchdog that does not lead its group kills the test alone, not the
  // group it shares (with meson itself, under meson --interactive)
  {
    const int status = wait_for(start_led("1", {"sleep", "20000"}));
    assert(WIFEXITED(status) && WEXITSTATUS(status) == kWatchdogKilled &&
           "the watchdog killed a group it does not lead");
  }

  // time the test spends stopped is not running time: stopped for 3 s of
  // its 4 s, it has run about 1 s of its 2 s budget. SIGSTOP of the group
  // stands in for a freeze of the build's cgroup, which a test cannot
  // make; either stops every task in it while the clocks run on.
  {
    int ready[2];
    const int piped = pipe(ready);
    assert(piped == 0);
    const pid_t pid = start("2", {"sleep", "4000", std::to_string(ready[1])});
    close(ready[1]);
    char byte = 0;
    const ssize_t got = read(ready[0], &byte, 1);
    assert(got == 1);
    close(ready[0]);
    sleep_ms(300);
    const int stopped = kill(-pid, SIGSTOP);
    assert(stopped == 0);
    sleep_ms(3000);
    const int resumed = kill(-pid, SIGCONT);
    assert(resumed == 0);
    const int status = wait_for(pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }

  printf("watchdog test ok\n");
}
