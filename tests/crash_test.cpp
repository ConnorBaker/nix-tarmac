// multi-process safety: concurrent writers must not lose data, and a
// SIGKILLed writer must never corrupt the store or lose synced batches
#include "test_tmp.hpp"

#include "tree.hpp"

#include <fcntl.h>
#include <lmdb.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
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
// runs once, before the next fsync of a regular file
static void (*before_file_fsync)() = nullptr;
// gets a byte when a non-blocking flock first finds the lock taken
static int notify_lock_wait = -1;
// runs once, before the next non-blocking flock
static void (*before_nb_flock)() = nullptr;
// run once, before and after the next index open
static void (*before_env_open)() = nullptr;
static void (*after_env_open)() = nullptr;
static bool watched_exists() {
  return watched != nullptr && access(watched, F_OK) == 0;
}
extern "C" __attribute__((visibility("default"))) int
msync(void *addr, size_t len, int flags) {
  if ((flags & MS_SYNC) != 0) {
    msyncs++;
    watched_at_msync = watched_exists();
  }
  return static_cast<int>(syscall(SYS_msync, addr, len, flags));
}
extern "C" __attribute__((visibility("default"))) int fsync(int fd) {
  struct stat st {};
  if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
    dir_fsyncs++;
    watched_at_dir_fsync = watched_exists();
  } else {
    fsyncs++;
    if (auto *hook = before_file_fsync) {
      before_file_fsync = nullptr;
      hook();
    }
  }
  return static_cast<int>(syscall(SYS_fsync, fd));
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
// a failed commit frees its txn, as LMDB's own does
extern "C" __attribute__((visibility("default"))) int
mdb_txn_commit(MDB_txn *txn) {
  static auto *const real = reinterpret_cast<int (*)(MDB_txn *)>(
      dlsym(RTLD_NEXT, "mdb_txn_commit"));
  watched_at_commit = watched_exists();
  if (fail_next_commit) {
    fail_next_commit = false;
    mdb_txn_abort(txn);
    return MDB_MAP_FULL;
  }
  return real(txn);
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
// commit() returned, so everything in the log must survive a SIGKILL
void killer(const std::string &dir, const std::string &log, uint64_t seed,
            bool durable) {
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    FILE *f = fopen(log.c_str(), "a");
    assert(f);
    auto cas = PackCas::open(dir);
    std::mt19937_64 rng(seed);
    std::vector<std::string> batch;
    for (unsigned i = 0;; i++) {
      std::string data = "crash-" + std::to_string(seed) + "-" +
                         std::to_string(i) + "-" +
                         std::to_string(rng());
      data.resize(100 + rng() % 8000, 'x');
      batch.push_back(cas->put(data));
      if (batch.size() >= 25) {
        if (durable)
          cas->sync();
        else
          cas->commit();
        for (auto &h : batch)
          fprintf(f, "%s\n", to_hex(h).c_str());
        fflush(f);
        assert(data_sync(fileno(f)) == 0);
        batch.clear();
      }
    }
  }
  std::mt19937_64 rng(seed ^ 0xdead);
  usleep(2000 + rng() % 60000);
  kill(pid, SIGKILL);
  int st;
  assert(waitpid(pid, &st, 0) == pid && WIFSIGNALED(st));
}

void crash_recovery(const std::string &dir) {
  std::string log = dir + "/../crash-log";
  remove(log.c_str());
  for (uint64_t round = 0; round < 20; round++) {
    killer(dir, log, round, round % 2 == 0);
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
    assert(cas->get(from_hex({line, 64}), out));
    n++;
  }
  fclose(f);
  assert(n > 0);
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
    assert(cas->get(hash, out));
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
    before_file_fsync = nullptr;
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

  // the new cache's writer commits between the stale writer's marker
  // write and its rename
  before_file_fsync = commit_fresh;
  (void)stale->put("stale, after the reset");
  try {
    stale->commit();
  } catch (const std::exception &err) {
    printf("replaced cache: stale commit failed: %s\n", err.what());
    fflush(stdout);
    assert(!"a stale writer's commit failed");
  }
  if (before_file_fsync != nullptr) {
    before_file_fsync = nullptr;
    commit_fresh();
  }
  assert(first_line(marker) == boot);
  // the stale writer's sync does not unmark the new cache's commit
  stale->sync();
  assert(first_line(marker) == boot);
  // nor does its compaction retire the new cache's pack, nor fail
  try {
    stale->compact([](std::string_view) { return true; });
  } catch (const std::exception &err) {
    printf("replaced cache: stale compaction failed: %s\n", err.what());
    fflush(stdout);
    assert(!"a stale writer's compaction failed");
  }
  // nor does its poison reset the new cache
  stale->poison();
  {
    auto fresh = PackCas::open(dir);
    assert(reads_back(*fresh, "fresh"));
  }
  stale.reset();

  // a reset waits for the writer flock: a writer holding it may be
  // using names in the directory
  auto fresh = PackCas::open(dir);
  (void)fresh->put("held");
  fresh->poison();
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
  // its pack is gone with its directory: a write fails
  const auto stale_put = [&](const char *data) {
    try {
      (void)stale->put(data);
      stale->commit();
    } catch (const std::system_error &err) {
      assert(err.code().value() == ENOENT);
    }
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
  }
  return static_cast<int>(syscall(SYS_unlinkat, dirfd, path, flags));
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
  // a writer of the cache now at dir commits between the stale writer's
  // marker write and its rename, as in replaced_cache
  before_file_fsync = commit_fresh;
  (void)stale->put("stale, after the reset");
  try {
    stale->commit();
  } catch (const std::exception &err) {
    printf("partial reset: stale commit failed: %s\n", err.what());
    fflush(stdout);
    assert(!"a stale writer's commit failed after a partial reset");
  }
  before_file_fsync = nullptr;
  printf("partial reset ok\n");
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
#endif

int main(int argc, char **argv) {
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
  moved_aside_swept(dir + "/sweep-test");
  sweep_spares_others(dir + "/spare-test");
  trailing_slash_reset(dir + "/slash-test");
#endif
  return 0;
}
