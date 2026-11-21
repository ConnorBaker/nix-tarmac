#ifndef NIX_TARMAC_PACK_CAS_HPP
#define NIX_TARMAC_PACK_CAS_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

auto blake3_hash(std::string_view data) -> std::string;

struct CorruptError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// LMDB index (BLAKE3 -> offset/length) plus an append-only packfile read
// through a shared mmap: lock-free reads from any process, writes batch
// under an flock. Blobs are LZ4-compressed when that shrinks them; hashes
// are over the raw content. compact() rewrites the pack as a new generation;
// readers remap, old mappings stay mapped until close.
class PackCas {
public:
  static auto open(const std::string &dir) -> std::unique_ptr<PackCas>;
  // the cache at dir, which it creates if absent, as one name: the
  // resolved directory, which open() uses. Equal for every spelling and
  // symlinked alias of the directory, and the same across resets; not
  // for a bind mount of it at another path. LMDB forbids opening one
  // environment twice in a process, so a process keeps one PackCas per key.
  static auto cache_key(const std::string &dir) -> std::string;
  ~PackCas();

  PackCas(const PackCas &) = delete;
  PackCas(PackCas &&) = delete;
  auto operator=(const PackCas &) -> PackCas & = delete;
  auto operator=(PackCas &&) -> PackCas & = delete;

  [[nodiscard]] auto put(std::string_view data) -> std::string;
  [[nodiscard]] auto get(std::string_view hash, std::string &out) -> bool;
  // out points into the mmap (raw blobs) or into scratch (compressed)
  [[nodiscard]] auto get_view(std::string_view hash, std::string &scratch,
                              std::string_view &out) -> bool;
  [[nodiscard]] auto size(std::string_view hash, uint64_t &out) -> bool;
  [[nodiscard]] auto has(std::string_view hash) -> bool;
  void sync();
  // like sync() but not durable: a process crash keeps the batch, an
  // unclean shutdown before the next sync() empties the cache
  void commit();

  // the next open() resets the cache, unless it was replaced since, and
  // reports `why`
  void poison(std::string_view why);

  auto pack_size() -> uint64_t;
  void compact(const std::function<bool(std::string_view)> &live);

  // separate dbi; writes join an open batch
  void meta_put(std::string_view key, std::string_view val);
  // one write txn: outside a batch, a commit costs an index flush
  void meta_put(const std::vector<std::pair<std::string, std::string>> &records);
  [[nodiscard]] auto meta_get(std::string_view key, std::string &out) -> bool;
  void meta_del(std::string_view key);
  void meta_del(const std::vector<std::string> &keys);
  void meta_scan(
      std::string_view prefix,
      const std::function<bool(std::string_view, std::string_view)> &callback);

private:
  static auto open_once(const std::string &dir, bool &reset)
      -> std::unique_ptr<PackCas>;
  struct Impl;
  static auto open_files(std::unique_ptr<Impl> impl, const std::string &dir)
      -> std::unique_ptr<PackCas>;
  explicit PackCas(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

#endif
