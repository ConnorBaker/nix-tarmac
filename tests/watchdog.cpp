// watchdog SECONDS TEST [ARGS...]: run TEST, and fail it if it has not
// returned after SECONDS of time in which it could run.
//
// meson's own test timeout cannot do this job in a Nix build. nixpkgs'
// mesonCheckPhase passes --timeout-multiplier=0, which turns every timeout
// off, so a test that deadlocks holds its builder forever. And a deadline on
// a clock would count time the build spends frozen: a build farm can freeze
// the whole build's cgroup (cgroup.freeze), and every clock keeps running
// while it is frozen, so a test that was frozen past its deadline would fail
// as soon as it thawed. The budget here is counted in ticks of a sleep loop
// instead. The watchdog is in the build's cgroup, so it stops ticking while
// the build is frozen and resumes with it; a freeze costs at most one tick.
//
// The test stays in the watchdog's process group, the one meson made for
// this test, so killing the group on timeout takes the test, its children
// and the watchdog together, and meson reports the test killed by SIGKILL.
// Otherwise the watchdog exits as the test did. meson --interactive and
// --gdb make no group for the test, so the group is meson's own; there
// (whenever the watchdog does not lead its group) a timeout kills the test
// alone, and its children may outlive it. --gdb and --wrapper wrap the
// watchdog, not the test.
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr long kTicksPerSecond = 10;

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s SECONDS TEST [ARGS...]\n", argv[0]);
    return 99;
  }
  const long budget = std::stol(argv[1]) * kTicksPerSecond;
  if (budget <= 0) {
    fprintf(stderr, "watchdog: budget must be positive: %s\n", argv[1]);
    return 99;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    perror("watchdog: fork");
    return 99;
  }
  if (pid == 0) {
    execv(argv[2], argv + 2);
    perror(argv[2]);
    _exit(127);
  }

  const timespec tick{.tv_sec = 0, .tv_nsec = 1'000'000'000 / kTicksPerSecond};
  for (long ticks = 0;; ++ticks) {
    int status = 0;
    const pid_t done = waitpid(pid, &status, WNOHANG);
    if (done < 0 && errno != EINTR) {
      perror("watchdog: waitpid");
      return 99;
    }
    if (done == pid) {
      if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
      }
      // die the way the test died, so meson names the signal
      const int sig = WTERMSIG(status);
      signal(sig, SIG_DFL);
      raise(sig);
      return 128 + sig;
    }
    if (ticks >= budget) {
      const bool leads_group = getpgrp() == getpid();
      fprintf(stderr,
              "watchdog: %s has not returned after %s s of running time; "
              "killing %s\n",
              argv[2], argv[1], leads_group ? "its process group" : "it");
      fflush(stderr);
      if (leads_group) {
        kill(0, SIGKILL);
        return 99; // unreachable: SIGKILL reached this process too
      }
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      signal(SIGKILL, SIG_DFL);
      raise(SIGKILL);
      return 128 + SIGKILL;
    }
    nanosleep(&tick, nullptr);
  }
}
