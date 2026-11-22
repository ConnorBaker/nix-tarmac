// multi-process safety: concurrent writers must not lose data, and a
// SIGKILLed writer must never corrupt the store or lose synced batches
#include "test_tmp.hpp"

#include "tree.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <lmdb.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <chrono>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#ifdef __linux__
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/syscall.h>

// interposes liblmdb's msync, the marker's fsyncs and mdb_txn_commit to
// count flushes, a directory's apart; an index flush, a directory fsync
// and an index commit each note whether `watched` (the marker) exists
static unsigned msyncs = 0;
static unsigned fsyncs = 0;
static unsigned dir_fsyncs = 0;
static const char *watched = nullptr;
static bool watched_at_msync = false;
static bool watched_at_dir_fsync = false;
static bool watched_at_commit = false;
static bool fail_next_commit = false;
// the next forced mdb_env_sync fails with this errno instead, if nonzero
static int fail_env_sync_with = 0;
// the next fsync of a regular file, of a directory, the next write(2)
// and the next renameat(2) fail with these errnos instead, if nonzero
static int fail_file_fsync_with = 0;
static int fail_dir_fsync_with = 0;
static int fail_write_with = 0;
static int fail_rename_with = 0;
// the next fdatasync fails with this errno instead, if nonzero
static int fail_fdatasync_with = 0;
// the next msync(MS_SYNC), an index flush, fails with this errno instead,
// if nonzero
static int fail_msync_with = 0;
// gets a byte when a non-blocking flock first finds the lock taken
static int notify_lock_wait = -1;
// runs once, before the next non-blocking flock
static void (*before_nb_flock)() = nullptr;
// run once, before and after the next index open
static void (*before_env_open)() = nullptr;
static void (*after_env_open)() = nullptr;
// the next open(2) or openat(2) of a path ending so fails with
// fail_open_errno
static const char *fail_open_suffix = nullptr;
static int fail_open_errno = 0;
// the latest syslog(3) message
static std::string logged;
// the next mdb_dbi_open returns this instead, if nonzero
static int fail_dbi_open_with = 0;
// while set, opening the boot id fails, as where /proc is missing
// (macOS): boot_id() then comes back empty
static bool hide_boot_id = false;
static constexpr const char *kBootId = "/proc/sys/kernel/random/boot_id";
// While it names a directory, a program this process execs must hold no
// fd on a file under it, nor on the boot id: after each open(2) or
// openat(2) that opened one, after each fopen(3), before each
// fdopendir(3), and where a test calls exec_probe(), a child execs this
// test with --held-fds, which lists /proc/self/fd. Each point stands for
// another thread's exec then. liblmdb's own opens call open, not the
// open64 defined below, so they are not points: LMDB's data.mdb gets
// FD_CLOEXEC only once mdb_env_open has returned.
static std::string exec_probe_root;
static unsigned exec_probes = 0;
static void exec_probe(const char *event) {
  if (exec_probe_root.empty()) {
    return;
  }
  const char *argv[] = {"crash_test", "--held-fds", exec_probe_root.c_str(),
                        nullptr};
  const pid_t pid = fork();
  if (pid == 0) {
    execv("/proc/self/exe", const_cast<char *const *>(argv));
    _exit(127);
  }
  int st = 0;
  if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st) ||
      WEXITSTATUS(st) != 0) {
    fprintf(stderr, "a program exec'd at %s holds a cache fd\n", event);
    abort();
  }
  exec_probes++;
}
// the --held-fds child: fails, naming them, if it holds such fds
static int held_fds(const char *root) {
  const std::string under = std::string(root) + "/";
  DIR *fds = opendir("/proc/self/fd");
  if (fds == nullptr) {
    perror("/proc/self/fd");
    return 2;
  }
  int held = 0;
  while (const dirent *entry = readdir(fds)) {
    std::array<char, 4096> target{};
    const std::string link = std::string("/proc/self/fd/") + entry->d_name;
    const ssize_t len =
        readlink(link.c_str(), target.data(), target.size() - 1);
    if (len <= 0) {
      continue;
    }
    const std::string_view path(target.data(), static_cast<size_t>(len));
    if (path.starts_with(under) || path == kBootId) {
      fprintf(stderr, "exec'd with fd %s: %.*s\n", entry->d_name,
              static_cast<int>(path.size()), path.data());
      held++;
    }
  }
  closedir(fds);
  return held == 0 ? 0 : 1;
}
static FILE *fopen_unless_boot_id(const char *name, const char *path,
                                  const char *mode) {
  if (hide_boot_id && strcmp(path, kBootId) == 0) {
    errno = ENOENT;
    return nullptr;
  }
  auto *real = reinterpret_cast<FILE *(*)(const char *, const char *)>(
      dlsym(RTLD_NEXT, name));
  FILE *file = real(path, mode);
  if (file != nullptr) {
    exec_probe("an fopen");
  }
  return file;
}
// a std::ifstream opens its file through libstdc++'s fopen, which under
// _FILE_OFFSET_BITS=64 may be spelled fopen64; each definition names its
// symbol
extern "C" __attribute__((visibility("default"))) FILE *
fopen_hook(const char *path, const char *mode) __asm__("fopen");
extern "C" FILE *fopen_hook(const char *path, const char *mode) {
  return fopen_unless_boot_id("fopen", path, mode);
}
extern "C" __attribute__((visibility("default"))) FILE *
fopen64_hook(const char *path, const char *mode) __asm__("fopen64");
extern "C" FILE *fopen64_hook(const char *path, const char *mode) {
  return fopen_unless_boot_id("fopen64", path, mode);
}
static bool watched_exists() {
  return watched != nullptr && access(watched, F_OK) == 0;
}
// fdatasync and a regular file's fsync record how much of the file a
// system crash can no longer lose. While `durable_dir` names a cache, the
// index's commits and flushes made without its marker, and the marker's
// removal, each require every pack there to be on disk to its end; the
// first pack that is not is described in `undurable`
static std::map<std::pair<dev_t, ino_t>, off_t> synced_size;
static const char *durable_dir = nullptr;
static std::string undurable;
// fsync(2): a file's fsync does not make its directory entry durable; a
// fsync of the directory does, for the entries it holds then. Each such
// fsync records them as (device, directory, file, name), so a name since
// unlinked, or given to another file, is not taken for durable. The same
// points require every nonempty pack's name among them; the first that is
// not is described in `unnamed`
using DurableName = std::tuple<dev_t, ino_t, ino_t, std::string>;
static std::set<DurableName> durable_names;
static std::string unnamed;
// before the directory's fsync: a name listed here is there when it runs
static auto names_in(int dir_fd) -> std::vector<DurableName> {
  std::vector<DurableName> names;
  struct stat dir_st {};
  const int list_fd = fcntl(dir_fd, F_DUPFD_CLOEXEC, 0);
  DIR *listing = list_fd < 0 ? nullptr : fdopendir(list_fd);
  if (fstat(dir_fd, &dir_st) != 0 || listing == nullptr) {
    if (listing != nullptr) {
      closedir(listing);
    } else if (list_fd >= 0) {
      close(list_fd);
    }
    return names;
  }
  rewinddir(listing);
  while (const dirent *entry = readdir(listing)) {
    struct stat st {};
    if (fstatat(dir_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(st.st_mode)) {
      names.emplace_back(st.st_dev, dir_st.st_ino, st.st_ino, entry->d_name);
    }
  }
  closedir(listing);
  return names;
}
static unsigned unmarked_index_points = 0;
static unsigned marker_removals = 0;
static void note_synced(int fd) {
  struct stat st {};
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
    synced_size[{st.st_dev, st.st_ino}] = st.st_size;
  }
}
static void require_packs_on_disk(const char *event) {
  struct stat dir_st {};
  if (stat(durable_dir, &dir_st) != 0) {
    throw std::runtime_error(std::string("stat ") + durable_dir);
  }
  for (const auto &entry : std::filesystem::directory_iterator(durable_dir)) {
    const std::string name = entry.path().filename();
    struct stat st {};
    if (!name.starts_with("pack.") || stat(entry.path().c_str(), &st) != 0) {
      continue;
    }
    const auto it = synced_size.find({st.st_dev, st.st_ino});
    const off_t synced = it == synced_size.end() ? 0 : it->second;
    if (st.st_size > synced && undurable.empty()) {
      undurable = std::string(event) + ": " + name + " is " +
                  std::to_string(st.st_size) + " bytes, " +
                  std::to_string(synced) + " of them synced";
    }
    if (st.st_size > 0 && unnamed.empty() &&
        !durable_names.contains(
            {st.st_dev, dir_st.st_ino, st.st_ino, name})) {
      unnamed = std::string(event) + ": " + name + " (" +
                std::to_string(st.st_size) +
                " bytes) was created after the directory's last fsync";
    }
  }
}
// an index commit or flush: unmarked, it may make durable entries that
// name the pack
static void index_point(const char *event) {
  if (durable_dir != nullptr &&
      access((std::string(durable_dir) + "/unsynced").c_str(), F_OK) != 0) {
    unmarked_index_points++;
    require_packs_on_disk(event);
  }
}
extern "C" __attribute__((visibility("default"))) int fdatasync(int fd) {
  if (const int err = fail_fdatasync_with; err != 0) {
    fail_fdatasync_with = 0;
    errno = err;
    return -1;
  }
  const int res = static_cast<int>(syscall(SYS_fdatasync, fd));
  if (res == 0) {
    note_synced(fd);
  }
  return res;
}
// before unlinkat (below) removes a file: its inode may be reused by
// the next pack
static void note_unlink(int dir_fd, const char *path) {
  struct stat st {};
  if (fstatat(dir_fd, path, &st, AT_SYMLINK_NOFOLLOW) == 0) {
    synced_size.erase({st.st_dev, st.st_ino});
  }
  if (struct stat dir_st {}; fstat(dir_fd, &dir_st) == 0) {
    std::erase_if(durable_names, [&](const DurableName &known) {
      return std::get<0>(known) == dir_st.st_dev &&
             std::get<1>(known) == dir_st.st_ino && std::get<3>(known) == path;
    });
  }
  if (durable_dir != nullptr && strcmp(path, "unsynced") == 0) {
    marker_removals++;
    require_packs_on_disk("marker removal");
  }
}
extern "C" __attribute__((visibility("default"))) int
msync(void *addr, size_t len, int flags) {
  if (const int err = fail_msync_with; err != 0 && (flags & MS_SYNC) != 0) {
    fail_msync_with = 0;
    errno = err;
    return -1;
  }
  if ((flags & MS_SYNC) != 0) {
    msyncs++;
    watched_at_msync = watched_exists();
    // mdb_env_sync (sync_all, compact) flushes the index by this msync
    // alone, outside mdb_txn_commit: under MDB_WRITEMAP it is what
    // guarantees an earlier NOSYNC or NOMETASYNC commit's meta page on
    // disk. Today each such flush either runs under the marker
    // (sync_all, which removes it after) or follows an unmarked commit
    // the hook below checks (compact); this check holds for a flush
    // that does neither
    index_point("index flush");
  }
  return static_cast<int>(syscall(SYS_msync, addr, len, flags));
}
extern "C" __attribute__((visibility("default"))) int fsync(int fd) {
  struct stat st {};
  std::vector<DurableName> names;
  if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
    if (const int err = fail_dir_fsync_with; err != 0) {
      fail_dir_fsync_with = 0;
      errno = err;
      return -1;
    }
    dir_fsyncs++;
    watched_at_dir_fsync = watched_exists();
    names = names_in(fd);
  } else {
    if (const int err = fail_file_fsync_with; err != 0) {
      fail_file_fsync_with = 0;
      errno = err;
      return -1;
    }
    fsyncs++;
  }
  const int res = static_cast<int>(syscall(SYS_fsync, fd));
  if (res == 0) {
    note_synced(fd);
    durable_names.insert(names.begin(), names.end());
  }
  return res;
}
extern "C" __attribute__((visibility("default"))) int
mdb_env_open(MDB_env *env, const char *path, unsigned flags, mdb_mode_t mode) {
  static auto *const real =
      reinterpret_cast<int (*)(MDB_env *, const char *, unsigned, mdb_mode_t)>(
          dlsym(RTLD_NEXT, "mdb_env_open"));
  if (auto *hook = before_env_open) {
    before_env_open = nullptr;
    hook();
  }
  const int res = real(env, path, flags, mode);
  if (auto *hook = after_env_open) {
    after_env_open = nullptr;
    hook();
  }
  return res;
}
static int open_or_fail(int dir_fd, const char *path, int flags, va_list ap) {
  const mode_t mode = (flags & (O_CREAT | O_TMPFILE)) != 0
                          ? static_cast<mode_t>(va_arg(ap, int))
                          : 0;
  if (hide_boot_id && strcmp(path, kBootId) == 0) {
    errno = ENOENT;
    return -1;
  }
  const size_t len = strlen(path);
  if (const char *suffix = fail_open_suffix;
      suffix != nullptr && len >= strlen(suffix) &&
      strcmp(path + len - strlen(suffix), suffix) == 0) {
    fail_open_suffix = nullptr;
    errno = fail_open_errno;
    return -1;
  }
  const int fd =
      static_cast<int>(syscall(SYS_openat, dir_fd, path, flags, mode));
  if (fd >= 0) {
    exec_probe("an open");
  }
  return fd;
}
// with _FILE_OFFSET_BITS=64, as meson builds, this defines open64
extern "C" __attribute__((visibility("default"))) int open(const char *path,
                                                           int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  const int fd = open_or_fail(AT_FDCWD, path, flags, ap);
  va_end(ap);
  return fd;
}
// likewise openat64
extern "C" __attribute__((visibility("default"))) int
openat(int dir_fd, const char *path, int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  const int fd = open_or_fail(dir_fd, path, flags, ap);
  va_end(ap);
  return fd;
}
static void log_message(const char *format, va_list ap) {
  char buf[1024];
  vsnprintf(buf, sizeof(buf), format, ap);
  logged = buf;
}
extern "C" __attribute__((visibility("default"))) void
syslog(int /*priority*/, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  log_message(format, ap);
  va_end(ap);
}
extern "C" __attribute__((visibility("default"))) void
__syslog_chk(int /*priority*/, int /*flag*/, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  log_message(format, ap);
  va_end(ap);
}
// glibc's fortified syslog() forwards its arguments here, not to
// __syslog_chk, where the compiler lacks __va_arg_pack but
// __fortify_use_clang && defined __USE_MISC holds (bits/syslog.h): Clang
extern "C" __attribute__((visibility("default"))) void
__vsyslog_chk(int /*priority*/, int /*flag*/, const char *format, va_list ap) {
  log_message(format, ap);
}
extern "C" __attribute__((visibility("default"))) int
mdb_dbi_open(MDB_txn *txn, const char *name, unsigned flags, MDB_dbi *dbi) {
  static auto *const real =
      reinterpret_cast<int (*)(MDB_txn *, const char *, unsigned, MDB_dbi *)>(
          dlsym(RTLD_NEXT, "mdb_dbi_open"));
  if (const int res = fail_dbi_open_with; res != 0) {
    fail_dbi_open_with = 0;
    return res;
  }
  return real(txn, name, flags, dbi);
}
// a failed commit frees its txn, as LMDB's own does
extern "C" __attribute__((visibility("default"))) int
mdb_txn_commit(MDB_txn *txn) {
  static auto *const real = reinterpret_cast<int (*)(MDB_txn *)>(
      dlsym(RTLD_NEXT, "mdb_txn_commit"));
  watched_at_commit = watched_exists();
  index_point("index commit");
  if (fail_next_commit) {
    fail_next_commit = false;
    mdb_txn_abort(txn);
    return MDB_MAP_FULL;
  }
  return real(txn);
}

extern "C" __attribute__((visibility("default"))) ssize_t
write(int fd, const void *buf, size_t len) {
  if (const int err = fail_write_with; err != 0) {
    fail_write_with = 0;
    errno = err;
    return -1;
  }
  return syscall(SYS_write, fd, buf, len);
}
extern "C" __attribute__((visibility("default"))) int
renameat(int old_dir, const char *old_path, int new_dir,
         const char *new_path) noexcept {
  if (const int err = fail_rename_with; err != 0) {
    fail_rename_with = 0;
    errno = err;
    return -1;
  }
  return static_cast<int>(
      syscall(SYS_renameat2, old_dir, old_path, new_dir, new_path, 0));
}
// an index flush that fails, as one meeting a disk's EIO does: LMDB's
// mdb_env_sync returns the errno. Only a forced one, as sync_all's and
// compact()'s are: mdb_txn_commit calls mdb_env_sync(env, 0) itself
// (mdb.c), and failing that fails the commit instead
extern "C" __attribute__((visibility("default"))) int
mdb_env_sync(MDB_env *env, int force) {
  static auto *const real = reinterpret_cast<int (*)(MDB_env *, int)>(
      dlsym(RTLD_NEXT, "mdb_env_sync"));
  if (const int res = fail_env_sync_with; res != 0 && force != 0) {
    fail_env_sync_with = 0;
    return res;
  }
  return real(env, force);
}

// the next non-blocking flock first compacts `compact_first` in a child:
// it lands between open()'s index read and its writer flock
static const char *compact_first = nullptr;
extern "C" __attribute__((visibility("default"))) int flock(int fd, int op) {
  if (auto *hook = before_nb_flock; hook != nullptr && (op & LOCK_NB) != 0) {
    before_nb_flock = nullptr;
    hook();
  }
  if ((op & LOCK_NB) != 0 && compact_first != nullptr) {
    const std::string dir = compact_first;
    compact_first = nullptr;
    const pid_t pid = fork();
    if (pid == 0) {
      PackCas::open(dir)->compact([](std::string_view) { return true; });
      _exit(0);
    }
    int st;
    assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
           WEXITSTATUS(st) == 0);
  }
  const int res = static_cast<int>(syscall(SYS_flock, fd, op));
  if (res < 0 && errno == EWOULDBLOCK && notify_lock_wait >= 0) {
    assert(write(notify_lock_wait, "w", 1) == 1);
    close(notify_lock_wait);
    notify_lock_wait = -1;
  }
  return res;
}
#endif

namespace {

int data_sync(int fd) {
#ifdef __APPLE__
  return fcntl(fd, F_BARRIERFSYNC);
#else
  return fdatasync(fd);
#endif
}

std::string blob_for(unsigned child, unsigned i) {
  std::string b = "child-" + std::to_string(child) + "-" + std::to_string(i);
  b.resize(100 + (i * 37) % 5000, static_cast<char>('a' + child));
  return b;
}

void concurrent_writers(const std::string &dir) {
  constexpr unsigned kChildren = 4, kBlobs = 500;
  for (unsigned c = 0; c < kChildren; c++) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      auto cas = PackCas::open(dir);
      for (unsigned i = 0; i < kBlobs; i++) {
        (void) cas->put(blob_for(c, i));
        if (i % 100 == 0)
          cas->sync();
      }
      cas->sync();
      _exit(0);
    }
  }
  for (unsigned c = 0; c < kChildren; c++) {
    int st;
    assert(wait(&st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
  }
  auto cas = PackCas::open(dir);
  std::string out;
  for (unsigned c = 0; c < kChildren; c++)
    for (unsigned i = 0; i < kBlobs; i++)
      assert(cas->get(blake3_hash(blob_for(c, i)), out) &&
             out == blob_for(c, i));
  printf("concurrent writers ok\n");
}

// child appends the batch's hashes to the log only after sync() or
// commit() returned, so everything in the log must survive a SIGKILL.
// The kill lands 2-62 ms after the child has logged `after` batches: with
// none, anywhere from its open() on; with some, the log has them however
// slowly the child runs, which a wall-clock delay alone never promised.
constexpr unsigned kBatch = 25;
void killer(const std::string &dir, const std::string &log, uint64_t seed,
            bool durable, unsigned after) {
  int logged[2];
  assert(pipe(logged) == 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(logged[0]);
    FILE *f = fopen(log.c_str(), "a");
    assert(f);
    auto cas = PackCas::open(dir);
    std::mt19937_64 rng(seed);
    std::vector<std::string> batch;
    unsigned batches = 0;
    for (unsigned i = 0;; i++) {
      std::string data = "crash-" + std::to_string(seed) + "-" +
                         std::to_string(i) + "-" +
                         std::to_string(rng());
      data.resize(100 + rng() % 8000, 'x');
      batch.push_back(cas->put(data));
      if (batch.size() >= kBatch) {
        if (durable)
          cas->sync();
        else
          cas->commit();
        for (auto &h : batch)
          fprintf(f, "%s\n", to_hex(h).c_str());
        fflush(f);
        assert(data_sync(fileno(f)) == 0);
        batch.clear();
        if (batches < after) {
          assert(write(logged[1], "b", 1) == 1);
          batches++;
        }
      }
    }
  }
  close(logged[1]);
  // a child that died first ends the pipe: read() returns 0
  for (unsigned b = 0; b < after; b++) {
    char c;
    assert(read(logged[0], &c, 1) == 1);
  }
  close(logged[0]);
  std::mt19937_64 rng(seed ^ 0xdead);
  usleep(2000 + rng() % 60000);
  kill(pid, SIGKILL);
  int st;
  assert(waitpid(pid, &st, 0) == pid && WIFSIGNALED(st));
}

void crash_recovery(const std::string &dir) {
  std::string log = dir + "/../crash-log";
  remove(log.c_str());
  // every pairing of sync() or commit() with 0, 1 or 2 batches awaited
  size_t awaited = 0;
  for (uint64_t round = 0; round < 20; round++) {
    const unsigned after = round % 3;
    killer(dir, log, round, round % 2 == 0, after);
    awaited += kBatch * after;
    if (round == 0) // tear a line on purpose, so the cut below always matters
      std::ofstream(log, std::ios::app) << "torn";
    // a kill can land mid-line: cut the log back to whole lines, or the
    // next round appends to the torn one
    std::ifstream in(log);
    std::string s(std::istreambuf_iterator<char>(in), {});
    assert(truncate(log.c_str(), s.rfind('\n') + 1) == 0); // npos + 1 == 0
  }

  auto cas = PackCas::open(dir);
  FILE *f = fopen(log.c_str(), "r");
  assert(f);
  char line[128];
  size_t n = 0;
  std::string out;
  while (fgets(line, sizeof(line), f)) {
    assert(strlen(line) == 65);
    const std::string hash = from_hex({line, 64});
    assert(cas->get(hash, out));
    // put() keys a blob by blake3_hash: get() finding the entry, and LZ4
    // decoding it, does not make what it returned that hash's blob
    assert(blake3_hash(out) == hash);
    n++;
  }
  fclose(f);
  assert(n >= awaited);
  printf("crash recovery: %zu committed hashes survived 20 kills\n", n);
}

std::string first_line(const std::string &path) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  return line;
}

// commit()'s marker names this boot until a sync(); open() keeps the
// cache under an empty marker and empties it under another boot's
void unsynced_marker(const std::string &dir) {
  const std::string marker = dir + "/unsynced";
  const std::string hash = blake3_hash("unsynced");
  std::string out;
  {
    auto cas = PackCas::open(dir);
    (void) cas->put("unsynced");
#ifdef __linux__
    const unsigned fsyncs_before = fsyncs;
    const unsigned dir_fsyncs_before = dir_fsyncs;
    watched = marker.c_str();
#endif
    cas->commit();
    const std::string boot = first_line("/proc/sys/kernel/random/boot_id");
    assert(first_line(marker) == boot);
#ifdef __linux__
    // the marker and its directory are on disk before the commit
    assert(fsyncs == fsyncs_before + 1);
    assert(dir_fsyncs == dir_fsyncs_before + 1);
    assert(watched_at_dir_fsync);
    assert(watched_at_commit);
    watched_at_msync = false;
    const unsigned msyncs_before_sync = msyncs;
#endif
    cas->sync();
#ifdef __linux__
    // the index was flushed, while the marker still stood
    assert(msyncs > msyncs_before_sync);
    assert(watched_at_msync);
    watched = nullptr;
#endif
    assert(access(marker.c_str(), F_OK) != 0);
    // a write outside a batch flushes the index once and needs no marker;
    // the batch commits after it stay unflushed
#ifdef __linux__
    unsigned before = msyncs;
#endif
    cas->meta_put("k", "v");
#ifdef __linux__
    assert(msyncs == before + 1);
#endif
    cas->meta_del("k");
    assert(access(marker.c_str(), F_OK) != 0);
#ifdef __linux__
    before = msyncs;
#endif
    (void) cas->put("after an owned commit");
    cas->commit();
#ifdef __linux__
    assert(msyncs == before);
#endif
    cas->sync();
    // a marker that cannot be written fails the commit and ends the batch
    std::filesystem::create_directory(marker + ".tmp");
    (void) cas->put("unmarked");
    bool threw = false;
    try {
      cas->commit();
    } catch (const std::exception &) {
      threw = true;
    }
    assert(threw);
    std::filesystem::remove(marker + ".tmp");
    const int lock = open((dir + "/lock").c_str(), O_RDWR | O_CLOEXEC);
    assert(lock >= 0);
    assert(flock(lock, LOCK_EX | LOCK_NB) == 0);
    close(lock);
    assert(!cas->has(blake3_hash("unmarked")));
#ifdef __linux__
    // so does a failed index commit
    fail_next_commit = true;
    (void) cas->put("uncommitted");
    threw = false;
    try {
      cas->commit();
    } catch (const std::exception &) {
      threw = true;
    }
    assert(threw);
    const int lock2 = open((dir + "/lock").c_str(), O_RDWR | O_CLOEXEC);
    assert(lock2 >= 0);
    assert(flock(lock2, LOCK_EX | LOCK_NB) == 0);
    close(lock2);
    assert(!cas->has(blake3_hash("uncommitted")));
#endif
  }
  // an empty marker (a writer killed before writing it) is rewritten
  std::ofstream(marker).close();
  {
    auto cas = PackCas::open(dir);
    assert(cas->get(hash, out) && out == "unsynced");
    (void) cas->put("over an empty marker");
    cas->commit();
    assert(first_line(marker) == first_line("/proc/sys/kernel/random/boot_id"));
  }
  std::ofstream(marker) << "another-boot\n";
  assert(!PackCas::open(dir)->get(hash, out));
  printf("unsynced marker ok\n");
}

// a flush of many touched roots is one write txn: one index flush
void touch_flush(const std::string &dir) {
  TreeStore store(PackCas::open(dir), std::chrono::hours(1),
                  std::chrono::hours(1), std::chrono::nanoseconds(0));
  for (unsigned i = 0; i < 50; i++)
    store.registerRoot(blake3_hash("root " + std::to_string(i)), false);
  for (unsigned i = 0; i < 50; i++)
    store.touchRoot(blake3_hash("root " + std::to_string(i)), false);
#ifdef __linux__
  const unsigned before = msyncs;
  store.sync();
  assert(msyncs == before + 1);
#endif
  printf("touch flush ok\n");
}

#ifdef __linux__
// open() removes other generations' packs: one a compaction made live
// after open() read the index must stay, or the next write starts a new
// pack at offset 0 under the entries that pointed into it
void compaction_during_open(const std::string &dir) {
  const std::string kept(50, 'k'), later(50, 'l'); // short: stored raw
  {
    auto cas = PackCas::open(dir);
    (void)cas->put(kept);
    cas->sync();
  }
  compact_first = dir.c_str();
  auto cas = PackCas::open(dir);
  assert(compact_first == nullptr);
  (void)cas->put(later);
  cas->sync();
  std::string out;
  assert(cas->get(blake3_hash(kept), out));
  assert(out == kept);
  printf("compaction during open ok\n");
}
#endif

} // namespace

// A process killed inside a read txn leaves its reader slot, and LMDB
// reuses no page freed after that snapshot: a cache with one such slot
// filled its 32 GiB index map with 26.7 GiB of free pages it could not
// reuse (MDB_MAP_FULL). After such a death, 5,000 writes must keep the
// index's high-water page low: record writes (their own write txns) and
// batches, each with its own dead reader. The batches commit without a
// sync: page reuse does not depend on flushing.
template <typename Each>
size_t pages_after_dead_reader(const std::string &dir, Each each) {
  const std::string index = dir + "/index";
  auto cas = PackCas::open(dir);
  int ready[2];
  assert(pipe(ready) == 0);
  const pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    MDB_env *env = nullptr;
    MDB_txn *txn = nullptr;
    if (mdb_env_create(&env) != 0 ||
        mdb_env_open(env, index.c_str(), MDB_RDONLY, 0644) != 0 ||
        mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn) != 0)
      _exit(1);
    assert(write(ready[1], "x", 1) == 1);
    pause();
  }
  close(ready[1]); // a child that fails closes the pipe: the read fails
  char token = 0;
  assert(read(ready[0], &token, 1) == 1);
  kill(pid, SIGKILL);
  assert(waitpid(pid, nullptr, 0) == pid);
  for (unsigned i = 0; i < 5000; i++)
    each(*cas, std::to_string(i));
  cas.reset(); // LMDB opens an environment once per process
  MDB_env *env = nullptr;
  MDB_envinfo info;
  assert(mdb_env_create(&env) == 0 &&
         mdb_env_open(env, index.c_str(), MDB_RDONLY | MDB_NOLOCK, 0644) == 0 &&
         mdb_env_info(env, &info) == 0);
  mdb_env_close(env);
  return info.me_last_pgno + 1;
}

void dead_reader_cleared(const std::string &root) {
  const size_t records = pages_after_dead_reader(
      root + "/dead-reader-records",
      [](PackCas &cas, const std::string &i) { cas.meta_put("k", i); });
  const size_t batches = pages_after_dead_reader(
      root + "/dead-reader-batches", [](PackCas &cas, const std::string &i) {
        (void)cas.put(i);
        cas.commit();
      });
  printf("dead reader: index high-water %zu pages after records, %zu after "
         "batches\n", records, batches);
  fflush(stdout);
  assert(records < 1024);
  assert(batches < 1024);
}

#ifdef __linux__
// another process's open() resets a cache: it replaces the directory, lock
// file included. A process holding the old cache flocks the old lock file,
// which does not exclude the new cache's writers, so it must name nothing
// in the directory: no marker, no marker removal, no poison, no pack.
static std::string replaced;

template <typename Body> void in_child(Body body) {
  const pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    body();
    _exit(0);
  }
  int st;
  assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

// a retired pack reads as corrupt
bool reads_back(PackCas &cas, const std::string &data) {
  std::string out;
  try {
    return cas.get(blake3_hash(data), out) && out == data;
  } catch (const CorruptError &) {
    return false;
  }
}

// whether f fails because the cache it used was replaced
template <typename F> auto throws_replaced(F f) -> bool {
  try {
    f();
  } catch (const ReplacedError &) {
    return true;
  }
  return false;
}

// an open that another process's reset lands in opens the new cache
std::unique_ptr<PackCas> open_during_reset(const std::string &dir) {
  try {
    return PackCas::open(dir);
  } catch (const std::exception &err) {
    printf("open during a reset failed: %s\n", err.what());
    fflush(stdout);
    assert(!"an open during a reset failed");
  }
  return nullptr;
}

// another process resets the cache and compacts it to a new generation,
// holding a blob only it has
static std::string compacted;
void reset_and_compact() {
  compacted += "+";
  in_child([] {
    std::ofstream(replaced + "/unsynced") << "another boot\n";
    auto cas = PackCas::open(replaced);
    (void)cas->put(compacted);
    cas->sync();
    cas->compact([](std::string_view) { return true; });
  });
}

// then flocks the replaced cache's lock file, as a writer of it would
static int replaced_lock = -1;
void reset_compact_and_hold() {
  reset_and_compact();
  assert(syscall(SYS_flock, replaced_lock, LOCK_EX) == 0);
}

// a writer of the new cache, its commit left unsynced
void commit_fresh() {
  in_child([] {
    auto cas = PackCas::open(replaced);
    (void)cas->put("fresh");
    cas->commit();
  });
}

void replaced_cache(const std::string &dir) {
  replaced = dir;
  const std::string marker = dir + "/unsynced";
  const std::string boot = first_line("/proc/sys/kernel/random/boot_id");
  auto stale = PackCas::open(dir);
  (void)stale->put("stale");
  stale->sync();
  std::ofstream(marker) << "another boot\n";
  in_child([&] { (void)PackCas::open(dir); });
  assert(access(marker.c_str(), F_OK) != 0);

  // a write of the stale writer's would land in the replaced cache, where
  // no later process finds it: it fails instead
  assert(throws_replaced([&] { (void)stale->put("stale, after the reset"); }));
  assert(access(marker.c_str(), F_OK) != 0);
  commit_fresh();
  assert(first_line(marker) == boot);
  // the stale writer's sync does not unmark the new cache's commit, nor
  // take it for its own
  assert(!throws_replaced([&] { stale->sync(); }));
  assert(first_line(marker) == boot);
  // nor does its compaction retire the new cache's pack, nor fail
  try {
    stale->compact([](std::string_view) { return true; });
  } catch (const std::exception &err) {
    printf("replaced cache: stale compaction failed: %s\n", err.what());
    fflush(stdout);
    assert(!"a stale writer's compaction failed");
  }
  // nor does its poison reset the new cache, nor report a failure: the
  // stale cache's directory is gone
  logged.clear();
  stale->poison("stale");
  assert(logged.empty());
  {
    auto fresh = PackCas::open(dir);
    assert(reads_back(*fresh, "fresh"));
  }
  stale.reset();

  // a reset waits for the writer flock: a writer holding it may be
  // using names in the directory
  auto fresh = PackCas::open(dir);
  (void)fresh->put("held");
  fresh->poison("held");
  int waiting[2];
  assert(pipe(waiting) == 0);
  const pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(waiting[0]);
    notify_lock_wait = waiting[1];
    (void)PackCas::open(dir);
    _exit(0);
  }
  close(waiting[1]);
  char token = 0;
  // EOF: the reset finished without waiting
  assert(read(waiting[0], &token, 1) == 1);
  close(waiting[0]);
  assert(reads_back(*fresh, "fresh"));
  fresh->commit();
  int st;
  assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
  assert(!PackCas::open(dir)->has(blake3_hash("fresh")));
  fresh.reset();

  // a reset between open()'s lock file and its flock: open() takes the
  // new cache, without retiring its live pack
  before_nb_flock = reset_and_compact;
  auto during = open_during_reset(dir);
  assert(before_nb_flock == nullptr);
  assert(reads_back(*during, compacted));
  during.reset();
  assert(reads_back(*PackCas::open(dir), compacted));

  // the same while a writer of the replaced cache holds its flock: open()
  // skips the orphan cleanup, and checks once the cache's files are open
  replaced_lock = open((dir + "/lock").c_str(), O_RDWR | O_CLOEXEC);
  assert(replaced_lock >= 0);
  before_nb_flock = reset_compact_and_hold;
  auto unlocked = open_during_reset(dir);
  close(replaced_lock);
  assert(before_nb_flock == nullptr);
  assert(reads_back(*unlocked, compacted));
  unlocked.reset();

  // a reset after open() opened the index: the index is the replaced
  // cache's, so open() takes the new cache
  after_env_open = reset_and_compact;
  auto late = open_during_reset(dir);
  assert(after_env_open == nullptr);
  assert(reads_back(*late, compacted));
  printf("replaced cache ok\n");
}

// a reset that moved the cache aside and has not yet removed it, while a
// new cache already stands at dir: an open that opened the directory
// before the move opens the new cache's index by name and would map the
// moved cache's pack through its descriptor
static std::string moved_aside;
void move_aside_and_refill() {
  assert(std::rename(replaced.c_str(), moved_aside.c_str()) == 0);
  in_child([] {
    auto cas = PackCas::open(replaced);
    (void)cas->put("refilled");
    cas->sync();
  });
}

void index_and_pack_of_one_cache(const std::string &dir) {
  replaced = dir;
  moved_aside = dir + ".moved";
  {
    auto cas = PackCas::open(dir);
    (void)cas->put("moved aside");
    cas->sync();
  }
  before_env_open = move_aside_and_refill;
  auto cas = open_during_reset(dir);
  assert(before_env_open == nullptr);
  assert(reads_back(*cas, "refilled"));
  printf("index and pack of one cache ok\n");
}

// a reset that opened the lock file before another process's reset
// replaced the cache: the new cache is not the one found broken, and the
// old lock file's flock does not exclude its writers, so the reset leaves
// it
void reset_of_replaced_cache(const std::string &dir) {
  replaced = dir;
  {
    auto first = PackCas::open(dir);
    (void)first->put("old");
    first->sync();
  }
  std::ofstream(dir + "/unsynced") << "another boot\n";
  before_nb_flock = reset_and_compact;
  auto after = open_during_reset(dir);
  assert(before_nb_flock == nullptr);
  assert(reads_back(*after, compacted));
  printf("reset of a replaced cache ok\n");
}

// a process of the replaced cache needs a generation it had not mapped:
// the new cache has a pack of that name, which it must neither read nor
// write
void stale_writer_new_pack(const std::string &dir) {
  replaced = dir;
  auto stale = PackCas::open(dir);
  in_child([] {
    auto cas = PackCas::open(replaced);
    (void)cas->put("gen0");
    cas->sync();
    cas->compact([](std::string_view) { return true; });
  });
  in_child([] {
    std::ofstream(replaced + "/unsynced") << "another boot\n";
    auto cas = PackCas::open(replaced);
    (void)cas->put("new0");
    cas->sync();
    cas->compact([](std::string_view) { return true; });
  });
  // the same offset in the new cache's pack holds "new0"
  std::string out;
  try {
    if (stale->get(blake3_hash("gen0"), out)) {
      assert(out == "gen0");
    }
  } catch (const std::runtime_error &) {
  }
  // its directory is gone: a write fails
  const auto stale_put = [&](const char *data) {
    assert(throws_replaced([&] {
      (void)stale->put(data);
      stale->commit();
    }));
  };
  stale_put("stale-one");
  in_child([] {
    auto cas = PackCas::open(replaced);
    (void)cas->put("fresh-two");
    cas->sync();
  });
  stale_put("stale-two");
  stale.reset();
  assert(reads_back(*PackCas::open(dir), "fresh-two"));
  printf("stale writer new pack ok\n");
}

// A reset's removal can fail part-way when an opener creates a file in the
// directory meanwhile: openers create the lock file, the index and the pack
// without the flock. Were the directory removed in place, it would then
// survive with a new lock file, dir_is would stay true for processes
// holding the old one, and their flock would no longer exclude its
// writers. The hook opens the cache before the reset's last rmdir.
static const char *remove_watch = nullptr;
static void (*before_remove)() = nullptr;
static bool remove_hook_fired = false;
static void remove_hook(const char *path) {
  if (before_remove == nullptr || remove_watch == nullptr) {
    return;
  }
  // dir itself or a name it was moved to: dir.reset.*
  const char *base = strrchr(remove_watch, '/');
  const char *name = path;
  if (const char *slash = strrchr(path, '/'); slash != nullptr) {
    name = slash + 1;
  }
  const bool match =
      base != nullptr && strncmp(name, base + 1, strlen(base + 1)) == 0;
  if (match) {
    auto *hook = before_remove;
    before_remove = nullptr;
    remove_hook_fired = true;
    hook();
  }
}
extern "C" __attribute__((visibility("default"))) int
remove(const char *path) noexcept {
  remove_hook(path);
  static auto *const real =
      reinterpret_cast<int (*)(const char *)>(dlsym(RTLD_NEXT, "remove"));
  return real(path);
}
extern "C" __attribute__((visibility("default"))) int
rmdir(const char *path) noexcept {
  remove_hook(path);
  return static_cast<int>(syscall(SYS_unlinkat, AT_FDCWD, path, AT_REMOVEDIR));
}
extern "C" __attribute__((visibility("default"))) int
unlinkat(int dirfd, const char *path, int flags) noexcept {
  if ((flags & AT_REMOVEDIR) != 0) {
    remove_hook(path);
  } else {
    note_unlink(dirfd, path);
  }
  return static_cast<int>(syscall(SYS_unlinkat, dirfd, path, flags));
}
// fdopendir(3) sets FD_CLOEXEC on the fd it is given, so the point is
// before it: the fd as its caller dup'd it
extern "C" __attribute__((visibility("default"))) DIR *fdopendir(int fd) {
  exec_probe("an fdopendir");
  static auto *const real =
      reinterpret_cast<DIR *(*)(int)>(dlsym(RTLD_NEXT, "fdopendir"));
  return real(fd);
}

static std::string partial_dir;
void open_during_remove() {
  in_child([] { (void)PackCas::open(partial_dir); });
}

void partial_reset(const std::string &dir) {
  replaced = dir;
  partial_dir = dir;
  auto stale = PackCas::open(dir);
  (void)stale->put("stale");
  stale->sync();
  std::ofstream(dir + "/unsynced") << "another boot\n";
  in_child([&] {
    remove_watch = partial_dir.c_str();
    before_remove = open_during_remove;
    (void)PackCas::open(dir);
    if (!remove_hook_fired) {
      printf("partial reset: the remove hook never fired\n");
      fflush(stdout);
      _exit(2);
    }
  });
  struct stat after{};
  printf("partial reset: directory %s after the reset\n",
         stat(dir.c_str(), &after) == 0 ? "survived" : "is gone");
  fflush(stdout);
  // the stale writer's write fails, as in replaced_cache
  assert(throws_replaced([&] { (void)stale->put("stale, after the reset"); }));
  printf("partial reset ok\n");
}

// a process that opened the cache before another process reset it: what
// it writes would land in the replaced cache, which no later process
// opens. It fails instead, and the new cache stays as the reset left it.
// Content reads still work: what the replaced cache holds under a hash
// is that hash's content.
void writes_after_reset(const std::string &dir) {
  replaced = dir;
  auto stale = PackCas::open(dir);
  const std::string before = stale->put("before the reset");
  stale->meta_put("record", "before the reset");
  stale->sync();
  std::ofstream(dir + "/unsynced") << "another boot\n";
  in_child([] { (void)PackCas::open(replaced); });
  assert(throws_replaced([&] { (void)stale->put("after the reset"); }));
  assert(throws_replaced([&] { stale->meta_put("record", "after"); }));
  assert(throws_replaced([&] { stale->meta_del("record"); }));
  std::string held;
  assert(stale->get(before, held) && held == "before the reset");
  assert(throws_replaced([&] { stale->check_live(); }));
  stale.reset();
  auto fresh = PackCas::open(dir);
  std::string out;
  assert(!fresh->has(blake3_hash("after the reset")));
  assert(!fresh->has(before));
  assert(!fresh->meta_get("record", out));
  printf("writes after a reset ok\n");
}

// a cache a reset moved aside and never finished removing, as after a
// kill between its rename and its removal: the next open removes it
void moved_aside_swept(const std::string &dir) {
  const std::string left = dir + ".reset.1.2";
  std::filesystem::create_directories(left + "/index");
  std::ofstream(left + "/pack.0") << "an 82 GB pack";
  (void)PackCas::open(dir);
  assert(access(left.c_str(), F_OK) != 0);
  printf("moved aside swept ok\n");
}

// the sweep removes only what a reset names <base>.reset.<pid>.<time>,
// and a symlink of that name without following it
void sweep_spares_others(const std::string &dir) {
  namespace fs = std::filesystem;
  fs::create_directories(dir);
  const std::string target = dir + "-target";
  fs::create_directories(target);
  std::ofstream(target + "/data") << "kept";
  fs::create_directory_symlink(target, dir + ".reset.9.9");
  for (const char *spared :
       {".resetx", ".reset.notes", ".reset.1.x", ".reset.1", ".reset..2"}) {
    fs::create_directories(dir + spared);
  }
  fs::create_directories(dir + "2.reset.1.2");
  fs::create_directories("x" + fs::path(dir).filename().string());
  (void)PackCas::open(dir);
  struct stat st{};
  assert(lstat((dir + ".reset.9.9").c_str(), &st) != 0);
  assert(access((target + "/data").c_str(), F_OK) == 0);
  for (const char *spared :
       {".resetx", ".reset.notes", ".reset.1.x", ".reset.1", ".reset..2"}) {
    assert(access((dir + spared).c_str(), F_OK) == 0);
  }
  assert(access((dir + "2.reset.1.2").c_str(), F_OK) == 0);
  printf("sweep spares others ok\n");
}

// a cache named with a trailing slash, as a tarmac:// store URI's path
// keeps it, still resets
void trailing_slash_reset(const std::string &dir) {
  const std::string slashed = dir + "/";
  {
    auto cas = PackCas::open(slashed);
    (void)cas->put("x");
    cas->sync();
  }
  std::ofstream(dir + "/unsynced") << "another boot\n";
  try {
    auto cas = PackCas::open(slashed);
    assert(!cas->has(blake3_hash("x")));
  } catch (const std::exception &err) {
    printf("trailing slash: open failed: %s\n", err.what());
    fflush(stdout);
    assert(!"a reset of a cache named with a trailing slash failed");
  }
  printf("trailing slash ok\n");
}

// open() names the directory the kernel resolves: through a symlink,
// link/../c is the symlink target's sibling, not dir/c
void dotdot_through_symlink(const std::string &dir) {
  namespace fs = std::filesystem;
  fs::create_directories(dir + "/sub/real");
  fs::create_directory_symlink(dir + "/sub/real", dir + "/link");
  {
    auto cas = PackCas::open(dir + "/link/../c/");
    (void)cas->put("beside the target");
    cas->sync();
  }
  assert(access((dir + "/sub/c/lock").c_str(), F_OK) == 0);
  assert(access((dir + "/c").c_str(), F_OK) != 0);
  printf("dotdot through symlink ok\n");
}

// a cache opened through a symlink to its directory: a reset replaces
// the directory the symlink names, so the symlink and the key survive it
void symlinked_dir_reset(const std::string &dir) {
  namespace fs = std::filesystem;
  fs::create_directories(dir + "/real");
  const std::string alias = dir + "/alias";
  fs::create_directory_symlink(dir + "/real", alias);
  {
    auto cas = PackCas::open(alias);
    (void)cas->put("x");
    cas->sync();
    cas->poison("probe");
  }
  const std::string key = PackCas::cache_key(alias);
  auto cas = PackCas::open(alias); // resets the poisoned cache
  assert(!cas->has(blake3_hash("x")));
  assert(fs::is_symlink(alias));
  assert(!fs::exists(dir + "/real/poison"));
  assert(PackCas::cache_key(alias) == key);
  printf("symlinked dir reset ok\n");
}

// every spelling and symlinked alias of a cache's directory is one key
void cache_keys(const std::string &dir) {
  namespace fs = std::filesystem;
  const std::string key = PackCas::cache_key(dir);
  fs::create_directory_symlink(dir, dir + "-alias");
  for (const std::string &spelling :
       {dir + "/", dir + "/.", dir + "//", dir + "-alias", dir + "-alias/"}) {
    assert(PackCas::cache_key(spelling) == key);
  }
  assert(PackCas::cache_key(dir + "-other") != key);
  // a reset replaces the directory, not the cache's name
  {
    auto cas = PackCas::open(dir);
    (void)cas->put("x");
    cas->sync();
  }
  std::ofstream(dir + "/unsynced") << "another boot\n";
  (void)PackCas::open(dir);
  assert(PackCas::cache_key(dir) == key);
  printf("cache keys ok\n");
}
#endif

#ifdef __linux__
// open() resets the cache only for corruption, and says why; any other
// error leaves it as it is
void open_errors_keep_cache(const std::string &dir) {
  {
    auto cas = PackCas::open(dir);
    (void)cas->put("kept");
    cas->sync();
  }
  logged.clear();
  fail_open_suffix = "lock";
  fail_open_errno = EACCES;
  bool threw = false;
  try {
    (void)PackCas::open(dir);
  } catch (const std::system_error &err) {
    threw = err.code().value() == EACCES;
  }
  assert(threw);
  assert(logged.empty());
  assert(reads_back(*PackCas::open(dir), "kept"));

  std::ofstream(dir + "/unsynced") << "another boot\n";
  (void)PackCas::open(dir);
  assert(logged.find("unsynced writes lost by an unclean shutdown") !=
         std::string::npos);
  assert(!PackCas::open(dir)->has(blake3_hash("kept")));

  // a poison names the corruption it found
  {
    auto cas = PackCas::open(dir);
    (void)cas->put("kept");
    cas->sync();
    cas->poison("pack truncated");
  }
  logged.clear();
  (void)PackCas::open(dir);
  assert(logged.find("poisoned: pack truncated") != std::string::npos);
  printf("open errors keep the cache ok\n");
}

// lmdb.h's corruption codes reset the cache; MDB_VERSION_MISMATCH, which
// another LMDB build returns for a sound index, does not
void lmdb_codes(const std::string &dir) {
  for (const int code : {MDB_INVALID, MDB_CORRUPTED, MDB_PAGE_NOTFOUND}) {
    {
      auto cas = PackCas::open(dir);
      (void)cas->put("kept");
      cas->sync();
    }
    logged.clear();
    fail_dbi_open_with = code;
    try {
      (void)PackCas::open(dir);
    } catch (const std::exception &err) {
      printf("lmdb codes: open failed: %s\n", err.what());
      fflush(stdout);
      assert(!"open() did not reset for a corruption code");
    }
    assert(logged.find(mdb_strerror(code)) != std::string::npos);
    assert(!PackCas::open(dir)->has(blake3_hash("kept")));
  }
  {
    auto cas = PackCas::open(dir);
    (void)cas->put("kept");
    cas->sync();
  }
  logged.clear();
  fail_dbi_open_with = MDB_VERSION_MISMATCH;
  bool threw = false;
  try {
    (void)PackCas::open(dir);
  } catch (const CorruptError &) {
  } catch (const std::runtime_error &err) {
    // it never heals by itself: the error says what will
    threw = std::string(err.what()).find("remove the cache directory") !=
            std::string::npos;
  }
  assert(threw);
  assert(logged.empty());
  assert(reads_back(*PackCas::open(dir), "kept"));
  printf("lmdb codes ok\n");
}
#endif

#ifdef __linux__
// The index names a blob by its place in a pack, and get() returns those
// bytes unchecked. A system crash keeps the pack's bytes only as far as
// its last fdatasync (fdatasync(2)), and keeps nothing in order across
// files, so an index that a crash keeps must name only synced pack bytes;
// a crash would otherwise leave entries that read back zeros, and no
// check that would reset the cache. Unsynced entries are allowed only
// under the marker, which makes open() reset the cache after a crash.
// So: a commit or flush of the index without the marker, and the
// marker's removal, come after the pack's fdatasync. sync() removes the
// marker; compact() commits the new pack's index without one. Nor does a
// crash keep a new file's name unless its directory was fsynced after the
// file was created (fsync(2)), so those points also come after the
// directory's fsync that holds each nonempty pack's name: compact()'s
// new pack would otherwise be named by an index that a crash keeps while
// the pack itself is gone.
void pack_durable_before_index(const std::string &dir) {
  synced_size.clear();
  undurable.clear();
  durable_names.clear();
  unnamed.clear();
  unmarked_index_points = marker_removals = 0;
  auto blob = [](unsigned i) {
    std::string b = "durable-" + std::to_string(i) + "-";
    b.resize(2000, static_cast<char>('a' + i % 26));
    return b;
  };
  {
    auto cas = PackCas::open(dir);
    durable_dir = dir.c_str();
    for (unsigned i = 0; i < 20; i++) {
      (void)cas->put(blob(i));
    }
    cas->commit();
    cas->sync();
    for (unsigned i = 20; i < 40; i++) {
      (void)cas->put(blob(i));
    }
    cas->commit();
    cas->compact([](std::string_view) { return true; });
    (void)cas->put(blob(40));
    cas->sync();
    durable_dir = nullptr;
  }
  if (!undurable.empty()) {
    printf("pack durable before index: %s\n", undurable.c_str());
    fflush(stdout);
  }
  assert(undurable.empty());
  if (!unnamed.empty()) {
    printf("pack name durable before index: %s\n", unnamed.c_str());
    fflush(stdout);
  }
  assert(unnamed.empty());
  // the checks ran: three syncs removed the marker, one of them
  // compact()'s own, and the compaction committed and flushed the index
  // without it
  assert(marker_removals == 3);
  assert(unmarked_index_points >= 2);
  auto cas = PackCas::open(dir);
  for (unsigned i = 0; i <= 40; i++) {
    assert(reads_back(*cas, blob(i)));
  }
  printf("pack durable before index ok\n");
}

// Without a boot id (macOS has no /proc) there is no marker, so every
// commit is synced (MDB_NOMETASYNC) and commit_batch() fdatasyncs the
// pack before it, and fsyncs the directory that names it: open() created
// the pack, and no marker's write fsynced the directory since
void boot_id_less_commit(const std::string &dir) {
  synced_size.clear();
  undurable.clear();
  durable_names.clear();
  unnamed.clear();
  unmarked_index_points = marker_removals = 0;
  auto blob = [](unsigned i) {
    std::string b = "bootless-" + std::to_string(i) + "-";
    b.resize(2000, 'b');
    return b;
  };
  hide_boot_id = true;
  {
    auto cas = PackCas::open(dir);
    durable_dir = dir.c_str();
    for (unsigned i = 0; i < 20; i++) {
      (void)cas->put(blob(i));
    }
    cas->commit();
    durable_dir = nullptr;
    // the boot id was hidden: commit() wrote no marker
    assert(access((dir + "/unsynced").c_str(), F_OK) != 0);
  }
  hide_boot_id = false;
  if (!undurable.empty()) {
    printf("boot-id-less commit: %s\n", undurable.c_str());
    fflush(stdout);
  }
  assert(undurable.empty());
  if (!unnamed.empty()) {
    printf("boot-id-less commit name: %s\n", unnamed.c_str());
    fflush(stdout);
  }
  assert(unnamed.empty());
  assert(unmarked_index_points >= 1);
  auto cas = PackCas::open(dir);
  for (unsigned i = 0; i < 20; i++) {
    assert(reads_back(*cas, blob(i)));
  }
  printf("boot-id-less commit ok\n");
}

// A compaction whose index flush fails after its commit has committed:
// the index names the new pack, so the pack stays. Removing it lets the
// next batch recreate it empty under that index, and serve later blobs'
// bytes for the compacted ones. The blobs are incompressible, so a wrong
// read returns other bytes rather than failing to decompress. A later
// sync succeeds, and Linux reports the lost writeback no more, so the
// compaction poisons the cache: the next open resets it.
void compaction_flush_failure(const std::string &dir) {
  auto blob = [](unsigned i) {
    std::mt19937 gen(i);
    std::string b(2000, '\0');
    for (auto &c : b) {
      c = static_cast<char>(gen());
    }
    return b;
  };
  {
    auto cas = PackCas::open(dir);
    for (unsigned i = 0; i < 20; i++) {
      (void)cas->put(blob(i));
    }
    cas->sync();
    fail_env_sync_with = EIO;
    bool threw = false;
    try {
      cas->compact([](std::string_view) { return true; });
    } catch (const std::runtime_error &err) {
      threw = std::string(err.what()).starts_with("env_sync compaction: ");
      if (!threw) {
        printf("compaction flush failure: compact() threw %s\n", err.what());
        fflush(stdout);
      }
    }
    // compact()'s own flush failed, after its commit
    assert(threw && fail_env_sync_with == 0);
    for (unsigned i = 20; i < 40; i++) {
      (void)cas->put(blob(i));
    }
    cas->sync();
    for (unsigned i = 0; i < 40; i++) {
      if (!reads_back(*cas, blob(i))) {
        printf("compaction flush failure: blob %u lost\n", i);
        fflush(stdout);
        assert(!"a blob was lost after the compaction's failed flush");
      }
    }
  }
  logged.clear();
  auto cas = PackCas::open(dir);
  if (logged.find("poisoned: the compacted index's flush failed: ") ==
      std::string::npos) {
    printf("compaction flush failure: the reopen logged \"%s\"\n",
           logged.c_str());
    fflush(stdout);
    assert(!"the cache did not reset after the compaction's failed flush");
  }
  printf("compaction flush failure ok\n");
}

// A poison records a corruption found in the cache, which a system crash
// does not undo and later writes can hide: a batch appends at a
// truncated pack's end, over offsets the index names. The next open()
// resets the cache and reports why (pack_cas.hpp), after a system crash
// too, so the poison's bytes and its name are on disk once poison()
// returns
void poison_durable(const std::string &dir) {
  auto cas = PackCas::open(dir);
  (void)cas->put("poisoned");
  cas->sync();
  synced_size.clear();
  durable_names.clear();
  cas->poison("found corrupt");
  struct stat dir_st {};
  struct stat st {};
  assert(stat(dir.c_str(), &dir_st) == 0 &&
         stat((dir + "/poison").c_str(), &st) == 0);
  const auto synced = synced_size.find({st.st_dev, st.st_ino});
  if (synced == synced_size.end() || synced->second != st.st_size) {
    printf("poison durable: its %lld bytes are not synced\n",
           static_cast<long long>(st.st_size));
    fflush(stdout);
    assert(!"the poison's bytes are not synced");
  }
  if (!durable_names.contains({st.st_dev, dir_st.st_ino, st.st_ino, "poison"})) {
    printf("poison durable: its name is not synced\n");
    fflush(stdout);
    assert(!"the poison's name is not synced");
  }
  cas.reset();
  logged.clear();
  (void)PackCas::open(dir);
  assert(logged.find("poisoned: found corrupt") != std::string::npos);
  printf("poison durable ok\n");
}

// poison() reports a step that fails and does not throw: its callers are
// handling the corruption. The poison is still placed, so the next open
// resets the cache, and says why unless the failed step wrote the reason.
// Placed in place, after a failed create or rename, its bytes are synced.
void poison_failures(const std::string &dir) {
  auto check = [](bool held, const std::string &step, const char *what) {
    if (!held) {
      printf("poison failures: %s: %s\n", step.c_str(), what);
      fflush(stdout);
      assert(!"a poison step's failure was mishandled");
    }
  };
  auto run = [&](const std::string &step, const char *report,
                 const char *reason, bool in_place, auto inject,
                 auto injected) {
    std::string sub = dir + "/" + step;
    std::ranges::replace(sub, ' ', '-');
    {
      auto cas = PackCas::open(sub);
      (void)cas->put("poisoned");
      cas->sync();
      synced_size.clear();
      logged.clear();
      inject();
      bool threw = false;
      try {
        cas->poison("found corrupt");
      } catch (const std::exception &) {
        threw = true;
      }
      check(injected(), step, "the injected failure never ran");
      struct stat st {};
      check(stat((sub + "/poison").c_str(), &st) == 0, step,
            "no poison was placed");
      check(!threw, step, "poison() threw");
      check(logged.find(report) != std::string::npos, step,
            "the failure was not reported");
      if (in_place) {
        const auto synced = synced_size.find({st.st_dev, st.st_ino});
        check(synced != synced_size.end() && synced->second == st.st_size,
              step, "the poison placed in place is not synced");
      }
    }
    logged.clear();
    (void)PackCas::open(sub);
    check(logged.find(reason) != std::string::npos, step,
          "the next open did not reset with the reason");
  };
  run("sync", "could not sync the poison", "poisoned: found corrupt", false,
      [] { fail_file_fsync_with = EIO; },
      [] { return fail_file_fsync_with == 0; });
  run("write", "could not write the poison", "poisoned: ", false,
      [] { fail_write_with = EIO; }, [] { return fail_write_with == 0; });
  run("directory sync", "could not sync the cache directory",
      "poisoned: found corrupt", false, [] { fail_dir_fsync_with = EIO; },
      [] { return fail_dir_fsync_with == 0; });
  run("create", "could not create the poison", "poisoned: found corrupt",
      true,
      [] {
        fail_open_suffix = ".tmp";
        fail_open_errno = EACCES;
      },
      [] { return fail_open_suffix == nullptr; });
  run("rename", "could not rename the poison into place",
      "poisoned: found corrupt", true, [] { fail_rename_with = EIO; },
      [] { return fail_rename_with == 0; });
  // ENOENT says the directory is gone, with the cache it held: nothing
  // to place, nothing to report
  {
    auto cas = PackCas::open(dir + "/gone");
    logged.clear();
    fail_rename_with = ENOENT;
    cas->poison("gone");
    check(fail_rename_with == 0 && logged.empty(), "rename ENOENT",
          "a gone directory's failure was reported");
  }
  printf("poison failures ok\n");
}

// A sync whose flush fails, of the pack or of the index: Linux reports a
// failed writeback once and may drop the unwritten pages, so the next
// sync succeeds and removes the marker over pages that never reached the
// disk. The failed sync poisons the cache, so the next open resets it.
void sync_flush_failure(const std::string &dir) {
  auto run = [&](const std::string &sub, const char *what, auto inject,
                 auto injected) {
    const std::string cache = dir + "/" + sub;
    {
      auto cas = PackCas::open(cache);
      (void)cas->put("flushed " + sub);
      inject();
      bool threw = false;
      try {
        cas->sync();
      } catch (const std::runtime_error &err) {
        threw = std::string(err.what()).starts_with(what);
        if (!threw) {
          printf("sync flush failure: %s: sync() threw %s\n", sub.c_str(),
                 err.what());
          fflush(stdout);
        }
      }
      assert(threw && injected());
      cas->sync();
      assert(access((cache + "/unsynced").c_str(), F_OK) != 0);
    }
    logged.clear();
    (void)PackCas::open(cache);
    if (logged.find(std::string("poisoned: a sync's flush failed: ") + what) ==
        std::string::npos) {
      printf("sync flush failure: %s: the reopen logged \"%s\"\n",
             sub.c_str(), logged.c_str());
      fflush(stdout);
      assert(!"the cache did not reset after a sync's failed flush");
    }
  };
  run("index", "env_sync: ", [] { fail_env_sync_with = EIO; },
      [] { return fail_env_sync_with == 0; });
  run("pack", "fdatasync pack: ", [] { fail_fdatasync_with = EIO; },
      [] { return fail_fdatasync_with == 0; });
  printf("sync flush failure ok\n");
}

// A commit under MDB_NOMETASYNC flushes the index map, and so can be the
// flush told of a failed writeback, as sync_all()'s can: a later sync
// then removes the marker over earlier commits' pages that never reached
// the disk. Each commit that flushes poisons the cache when its flush
// fails, so the next open resets it; a commit that fails with an LMDB
// code (MDB_MAP_FULL) resets nothing.
static std::string other_writer_dir;
void commit_flush_failure(const std::string &dir) {
  auto check = [](bool held, const std::string &route, const char *what) {
    if (!held) {
      printf("commit flush failure: %s: %s\n", route.c_str(), what);
      fflush(stdout);
      assert(!"a commit's failed flush was mishandled");
    }
  };
  auto fails = [&](const std::string &route, auto body) {
    try {
      body();
    } catch (const std::runtime_error &err) {
      if (std::string(err.what()).starts_with("txn_commit: ")) {
        return true;
      }
      printf("commit flush failure: %s: threw %s\n", route.c_str(),
             err.what());
      fflush(stdout);
    }
    return false;
  };
  auto resets = [&](const std::string &cache) {
    logged.clear();
    (void)PackCas::open(cache);
    return logged.find("poisoned: an index commit failed: ") !=
           std::string::npos;
  };
  // an unsynced commit, then an owned meta commit whose flush fails, as
  // another thread's meta_put or meta_del can land between a batch's
  // commit and its sync_all(); then a sync that succeeds
  auto meta = [&](const std::string &route, auto op) {
    const std::string cache = dir + "/" + route;
    {
      auto cas = PackCas::open(cache);
      cas->meta_put("key", "val");
      (void)cas->put("unsynced " + route);
      cas->commit();
      fail_msync_with = EIO;
      check(fails(route, [&] { op(*cas); }) && fail_msync_with == 0, route,
            "the commit's flush did not fail");
      cas->sync();
      check(access((cache + "/unsynced").c_str(), F_OK) != 0, route,
            "the marker stayed");
    }
    check(resets(cache), route, "the next open did not reset");
  };
  meta("meta_put", [](PackCas &cas) { cas.meta_put("key", "other"); });
  meta("meta_del", [](PackCas &cas) { cas.meta_del("key"); });

  // compact(): another process's unsynced commit lands between its
  // leading sync() and its writer lock
  {
    const std::string cache = dir + "/compact";
    {
      auto cas = PackCas::open(cache);
      (void)cas->put("compacted");
      cas->sync();
      other_writer_dir = cache;
      before_nb_flock = [] {
        const pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
          auto other = PackCas::open(other_writer_dir);
          (void)other->put("the other process's");
          other->commit();
          _exit(0);
        }
        int st;
        assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
               WEXITSTATUS(st) == 0);
        fail_msync_with = EIO;
      };
      const bool failed = fails("compact", [&] {
        cas->compact([](std::string_view) { return true; });
      });
      check(failed && before_nb_flock == nullptr && fail_msync_with == 0,
            "compact", "the commit's flush did not fail");
      cas->sync();
      check(access((cache + "/unsynced").c_str(), F_OK) != 0, "compact",
            "the marker stayed");
    }
    check(resets(cache), "compact", "the next open did not reset");
  }

  // without a boot id every commit flushes, a batch's too: the earlier
  // commit's meta page is unsynced until the next commit's flush
  {
    const std::string cache = dir + "/bootless";
    hide_boot_id = true;
    {
      auto cas = PackCas::open(cache);
      (void)cas->put("bootless one");
      cas->commit();
      (void)cas->put("bootless two");
      fail_msync_with = EIO;
      check(fails("batch", [&] { cas->commit(); }) && fail_msync_with == 0,
            "batch", "the commit's flush did not fail");
      (void)cas->put("bootless three");
      cas->commit();
    }
    hide_boot_id = false;
    check(resets(cache), "batch", "the next open did not reset");
  }

  // and so does an open's, which writes a new cache's format record
  {
    const std::string cache = dir + "/open";
    hide_boot_id = true;
    fail_msync_with = EIO;
    check(fails("open", [&] { (void)PackCas::open(cache); }) &&
              fail_msync_with == 0,
          "open", "the commit's flush did not fail");
    hide_boot_id = false;
    check(resets(cache), "open", "the next open did not reset");
  }

  // a commit that fails with an LMDB code flushed nothing
  {
    const std::string cache = dir + "/map-full";
    {
      auto cas = PackCas::open(cache);
      (void)cas->put("map full");
      cas->commit();
      fail_next_commit = true;
      check(fails("map full", [&] { cas->meta_put("key", "val"); }) &&
                !fail_next_commit,
            "map full", "the commit did not fail");
      cas->sync();
    }
    logged.clear();
    (void)PackCas::open(cache);
    check(logged.find("poisoned") == std::string::npos &&
              access((cache + "/poison").c_str(), F_OK) != 0,
          "map full", "the next open reset");
  }
  printf("commit flush failure ok\n");
}

// A program a cache's process execs holds none of the cache's fds, at any
// point of open(), a batch, a commit, a compaction, a poison or the reset
// it brings: none, LMDB's data.mdb among them, lacks FD_CLOEXEC
void exec_holds_no_cache_fd(const std::string &root) {
  const std::string dir = root + "/cache";
  assert(mkdir(root.c_str(), 0755) == 0);
  {
    auto cas = PackCas::open(dir);
    (void)cas->put(std::string(5000, 'a'));
    cas->sync();
  }
  std::array<char, PATH_MAX> resolved{};
  assert(realpath(root.c_str(), resolved.data()) != nullptr);
  exec_probe_root = resolved.data();
  exec_probe("a test's start");
  const unsigned before_open = exec_probes;
  auto cas = PackCas::open(dir);
  const unsigned in_open = exec_probes - before_open;
  exec_probe("an open cache");
  (void)cas->put(std::string(5000, 'b'));
  exec_probe("an open batch");
  cas->commit(); // writes the unsynced marker
  (void)cas->put(std::string(5000, 'c'));
  cas->commit(); // reads it
  cas.reset();
  cas = PackCas::open(dir); // reads it
  assert(access((dir + "/unsynced").c_str(), F_OK) == 0);
  exec_probe("a reopened cache");
  cas->compact([](std::string_view) { return true; });
  exec_probe("a compacted cache");
  cas->poison("exec probe"); // writes poison.tmp
  cas.reset();
  cas = PackCas::open(dir); // reads the poison, and resets under the lock
  assert(access((dir + "/poison").c_str(), F_OK) != 0);
  exec_probe("a reset cache");
  cas.reset();
  exec_probe_root.clear();
  // open()'s own points: the boot id's, the lock's and the pack's opens
  assert(in_open >= 3);
  printf("exec holds no cache fd ok: %u exec probes\n", exec_probes);
}
#endif

int main(int argc, char **argv) {
#ifdef __linux__
  if (argc == 3 && strcmp(argv[1], "--held-fds") == 0) {
    return held_fds(argv[2]);
  }
#endif
  std::string dir = argc > 1 ? argv[1] : make_test_dir("packcas-crash-test");
  concurrent_writers(dir);
  crash_recovery(dir);
  dead_reader_cleared(dir);
  unsynced_marker(dir + "/unsynced-test");
  touch_flush(dir + "/touch-test");
#ifdef __linux__
  compaction_during_open(dir + "/compact-test");
  replaced_cache(dir + "/replaced-test");
  index_and_pack_of_one_cache(dir + "/one-cache-test");
  reset_of_replaced_cache(dir + "/replaced-reset-test");
  stale_writer_new_pack(dir + "/stale-pack-test");
  partial_reset(dir + "/partial-reset-test");
  writes_after_reset(dir + "/writes-after-reset-test");
  moved_aside_swept(dir + "/sweep-test");
  sweep_spares_others(dir + "/spare-test");
  trailing_slash_reset(dir + "/slash-test");
  dotdot_through_symlink(dir + "/dotdot-test");
  cache_keys(dir + "/key-test");
  symlinked_dir_reset(dir + "/symlink-test");
  open_errors_keep_cache(dir + "/open-errors-test");
  lmdb_codes(dir + "/lmdb-codes-test");
  pack_durable_before_index(dir + "/durable-test");
  boot_id_less_commit(dir + "/bootless-test");
  compaction_flush_failure(dir + "/compact-flush-test");
  poison_durable(dir + "/poison-durable-test");
  poison_failures(dir + "/poison-failures-test");
  sync_flush_failure(dir + "/sync-flush-test");
  commit_flush_failure(dir + "/commit-flush-test");
  exec_holds_no_cache_fd(dir + "/exec-test");
#endif
  return 0;
}
