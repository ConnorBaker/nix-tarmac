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
#include <fstream>
#include <iterator>
#include <random>
#include <string>
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
  }
  return static_cast<int>(syscall(SYS_fsync, fd));
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
    assert(cas->get(from_hex({line, 64}), out));
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

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : make_test_dir("packcas-crash-test");
  concurrent_writers(dir);
  crash_recovery(dir);
  dead_reader_cleared(dir);
  unsynced_marker(dir + "/unsynced-test");
  touch_flush(dir + "/touch-test");
  return 0;
}
