// corruption test: truncated/flipped pack must error or misread, never crash
#include "test_tmp.hpp"

#include "pack_cas.hpp"
#include "tree.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <exception>
#include <memory>
#include <random>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : make_test_dir("packcas-corrupt-test");

  std::mt19937_64 rng(7);
  std::vector<std::string> hashes;
  {
    auto cas = PackCas::open(dir);
    for (int i = 0; i < 1000; i++) {
      std::string data(rng() % 4096, char('a' + i % 26));
      hashes.push_back(cas->put(data));
    }
    cas->sync();
  }

  // flip random bytes in the pack: gets must not crash
  {
    int fd = open((dir + "/pack.0").c_str(), O_RDWR);
    assert(fd >= 0);
    off_t size = lseek(fd, 0, SEEK_END);
    for (int i = 0; i < 100; i++) {
      char c = rng();
      if (pwrite(fd, &c, 1, rng() % size) != 1)
        return 1;
    }
    close(fd);
    auto cas = PackCas::open(dir);
    std::string out;
    size_t rejected = 0;
    for (auto &h : hashes) {
      try {
        (void) cas->get(h, out);
      } catch (const CorruptError &) {
        rejected++;
      }
    }
    printf("bit flips: %zu rejected\n", rejected);
  }

  // truncate the pack: out-of-bounds entries must throw, not SIGBUS
  {
    if (truncate((dir + "/pack.0").c_str(), 100) != 0)
      return 1;
    auto cas = PackCas::open(dir);
    std::string out;
    size_t errors = 0, ok = 0;
    for (auto &h : hashes) {
      try {
        if (cas->get(h, out))
          ok++;
      } catch (std::exception &) {
        errors++;
      }
    }
    assert(errors > 0);
    printf("corrupt test ok: %zu bounds errors, %zu reads\n", errors, ok);
  }

  // truncation while the store is open: get() must throw, never SIGBUS
  {
    std::string dir2 = dir + "/live";
    auto cas = PackCas::open(dir2);
    std::vector<std::string> hs;
    for (int i = 0; i < 100; i++)
      hs.push_back(cas->put(std::string(3000, char('a' + i % 26))));
    cas->sync();
    if (truncate((dir2 + "/pack.0").c_str(), 50) != 0)
      return 1;
    std::string out;
    size_t errors = 0;
    for (auto &h : hs) {
      try {
        (void) cas->get(h, out);
      } catch (std::exception &) {
        errors++;
      }
    }
    assert(errors > 0);
    printf("live truncation ok: %zu errors\n", errors);
  }

  // trashed index: open() must wipe and start fresh
  {
    std::string dir3 = dir + "/heal";
    {
      auto cas = PackCas::open(dir3);
      (void) cas->put("hello");
      cas->sync();
    }
    int fd = open((dir3 + "/index/data.mdb").c_str(), O_WRONLY);
    assert(fd >= 0);
    std::string junk(4096, char(0x5a));
    assert(pwrite(fd, junk.data(), junk.size(), 0) > 0);
    close(fd);
    std::unique_ptr<PackCas> cas;
    try {
      cas = PackCas::open(dir3);
    } catch (const std::exception &err) {
      printf("open of a trashed index failed: %s\n", err.what());
    }
    assert(cas && "open() resets a trashed index");
    std::string h = cas->put("fresh start");
    std::string out;
    assert(cas->get(h, out) && out == "fresh start");
    printf("self-heal ok\n");
  }

  // a tree entry's name must be one file name: every name tarmac writes is
  // one (a tarball's paths are split at '/', and Nix names directory entries
  // with nix::Filename), so any other is a corrupt tree object, refused
  // before it reaches a NAR or a directory listing
  {
    TreeStore store(PackCas::open(dir + "/names"));
    const std::string blob = store.putBlob("x");
    auto read_one = [&](const std::string &name) -> bool {
      const std::string tree = store.putTree({{name, 'r', blob}});
      try {
        (void)store.readTree(tree);
        return true;
      } catch (const CorruptError &) {
        return false;
      }
    };
    for (const char *name : {"a", "...", ".a", "a b", "~nix~case~hack~1"})
      assert(read_one(name));
    for (const std::string &name :
         {std::string(), std::string("."), std::string(".."),
          std::string("a/b"), std::string("/"), std::string("a\0b", 3)})
      assert(!read_one(name));
    printf("tree entry names ok\n");
  }
  return 0;
}
