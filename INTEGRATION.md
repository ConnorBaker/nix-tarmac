# The `integration` branch

This branch is upstream `main` (Mic92/nix-tarmac) with this fork's topic branches merged into it, one `git merge --no-ff` per branch, in the order below. It is what the farm builds and runs; it is not meant to go upstream as a whole. Each topic branch is its own unit and goes upstream (or not) on its own.

Each topic branch is a linear stack of commits on upstream `main`, or on the topic it depends on ("Stacked on"). A topic that depends on two or more topics sits on a merge of them; the reason is in the table. When two independent topics touch the same lines, the integration merge of the later one resolves it, and that merge commit's message names the files.

`integ/<name>`: the integration copy of a change that also exists as an upstream pull request or upstream commit in another form (rebased, reworded, or merged upstream after this base). The pull request branch keeps its own name and is not changed by this branch.

Coverage: each topic branch below was built alone, x86_64-linux only, as `checks.x86_64-linux.tests`, and passed. That is all that was run per topic. The full check set (all systems, VM tests, clang-tidy) gates this integration branch, not each topic alone; run it on a topic before that topic goes upstream.


Base `main` commit: `8ae9fd6b69568abd4bf3a060e04f3b5fc8493a33`

## Merged branches

| Merge order | Branch | Head sha merged | Stacked on | Why stacked | Description | Upstream PR |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `gc-test-root-stamps` | `bddd1ba4a4ebc5bf249ce7fa58a1dce4a4738cfb` |  |  | gc_test ages roots by their stamps instead of a sleep (first commit of PR 33; not the nometasync change of that PR) | PR 33 (open; its second commit, the nometasync change, is not integrated) |
| 2 | `eval-store-unsynced-commits` | `13faa46744bea4f64dae851178c1eac83eeed5ab` | `clear-dead-readers` | its crash_test changes the dead-reader batches | Eval-store writes commit without syncing; an unsynced marker plus MDB_NOMETASYNC for commits outside a batch (stage 1) |  |
| 3 | `crash-test-torn-line` | `31c2c0125ea5d3b0ffc3f26f8aada59d0ab18e05` |  |  | crash_test cuts the log back to whole lines after each kill. Merged as the PR head; the PR head also adds `#include <fstream>`, which integration already has from eval-store-unsynced-commits, so the merge drops that line | PR 34 (open) |
| 4 | `restore-path-sink` | `4c07a0e69779b70297fcf2cb2d219d41a8121ce3` |  |  | Restore dumps through Nix's restorePath when it has one (the PR head; integration's copy differs only in context) | PR 35 (open) |
| 5 | `open-generation-under-flock` | `1138f9214ac4f19faa73b93aa1b98f6a7a864c51` | `eval-store-unsynced-commits` |  | Read the live generation under the writer flock (open vs compaction race) |  |
| 6 | `reset-keeps-writer-lock` | `ca65616018a0d93561d09794485aae654eb0f95b` | `open-generation-under-flock` |  | Keep the writer lock's exclusion across a cache reset |  |
| 7 | `reset-only-for-corruption` | `bf52bfbf895cff8ebbd4d9af39d6fa27db37402e` | `reset-keeps-writer-lock` |  | Reset the cache only for corruption, and say why |  |
| 8 | `one-store-per-cache` | `c730b0ebadd0e2ff29e26ac4e7ba72918e896182` | `reset-only-for-corruption` | its crash_test calls poison(reason) from reset-only-for-corruption (does not build without it) | One store per cache in a process, whatever its spelling |  |
| 9 | `test-watchdog` | `f45f016cc7fa4b97446dd682f5358385901a68cd` |  |  | Fail a test that hangs, counting only time it could run |  |
| 10 | `model-test-skip-syncs` | `a66aaf0a4d948e9ddaa2753e54f835b713b2f8db` | `test-watchdog` |  | model_test stops waiting for syncs nothing there can observe |  |
| 11 | `nixpkgs-builds-nix-master` | `2be12d931498c2893f90b6c7c8a8880fcfc97a49` |  |  | flake.lock: nixpkgs that builds Nix master |  |
| 12 | `crash-test-pack-before-index` | `00dd752a634a235a660f9fe2c2d04e3bd32054bd` | `one-store-per-cache` |  | crash_test requires the pack on disk before the index names it |  |
| 13 | `compact-dir-fsync` | `bc9701f7e3b24bd9e48b0a73f3a1c304d0e6d9dc` | `crash-test-pack-before-index` |  | fsync the directory before an unmarked commit names a new pack |  |
| 14 | `crash-test-clang` | `8547135831cb09b5df92ed8a9f80a0263f2573fb` | `one-store-per-cache` | reset-only-for-corruption: the syslog capture serves that topic's open_errors_keep_cache test; one-store-per-cache: the Clang check sits beside its one-cache check | crash_test captures Clang's __vsyslog_chk; a flake check of the tests built with Clang |  |
| 15 | `compact-flush-failure` | `7c9c34feca9b46e1824c8a95f16d5368e65f7380` | `compact-dir-fsync` | reset-only-for-corruption: poison() takes the reason added there; compact-dir-fsync: changes the compact() lines added there | Keep a poison through a crash; keep the compacted pack and reset when the index flush fails after the commit |  |
| 16 | `poison-on-flush-failure` | `6b8f12c54f43c7d450fc550cbf3df1a4e51a880c` | `compact-flush-failure` |  | Poison the cache when an index or pack flush fails |  |
| 17 | `refuse-replaced-cache` | `cf89aecf27c3814274cfe78ccde854435cd13243` | `one-store-per-cache` | changes the reset and Ctx code of one-store-per-cache | Refuse writes and validity answers from a cache a reset replaced, with its C API test |  |
| 18 | `crash-recovery-kill-after-batches` | `3bd7887b0cb13cff4a1c7c7e80e715b685ff5aa4` | `eval-store-unsynced-commits`, `crash-test-torn-line` | eval-store-unsynced-commits: the crash_recovery harness it changes; crash-test-torn-line: the log-cutting loop it changes (does not build without it) | crash_recovery kills its writers after their committed batches |  |
| 19 | `crash-recovery-check-content` | `e5d4b18f3e7332c515bf5749c07b8686d9ff62d6` | `eval-store-unsynced-commits` |  | crash_recovery checks recovered blobs against their hashes |  |
| 20 | `tests-compare-read-bytes` | `5efb60fe0c1b3a77c0da47583d768bd67e267d97` | `refuse-replaced-cache` |  | Tests compare blobs read back with the bytes stored |  |
| 21 | `exit-fails-after-reset` | `14ee0425ed8fccf3000274203159492f9a76bb9f` | `refuse-replaced-cache` |  | Fail at exit when a reset replaced the cache a path came from |  |
| 22 | `cloexec-cache-fds` | `885bd52f685fa67dc8d0360c7000c21e822787a8` | `compact-dir-fsync`, `refuse-replaced-cache` | sets close-on-exec on descriptors opened by code of each of the three | Close-on-exec on every fd a cache holds |  |

Already merged before the first row (by the merges at the start of this branch): `query-valid-paths` (`4c75afafc4`), `clear-dead-readers` (`88847c4228`).


7 of 22 merges resolve overlaps; each names its files in its message.

The `integration: describe the merged branches` commit only adds or updates this file and `maintainers/integration-status.sh`.

`maintainers/integration-status.sh [remote]` compares each row with the remote: branch head, and whether that head is merged here.

