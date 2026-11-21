#!/usr/bin/env bash
# one_cache.sh PLUGIN_DIR WORK: a process opens each cache's LMDB
# environment once (plugin/ctx.hh), counted with strace, without network:
#  - the fetcher's default directory and a tarmac:// eval store naming it
#    with a trailing slash;
#  - a cache directory reached through a symlink, reset during the run.
set -euo pipefail
plugin=$1
work=$2
mkdir -p "$work/src"
echo hello >"$work/src/file.txt"
tar -C "$work" -czf "$work/src.tar.gz" src
fetch() { echo "(builtins.fetchTree { type = \"tarball\"; url = \"file://$work/src.tar.gz$1\"; }).narHash"; }
common=(--extra-experimental-features 'nix-command flakes' --impure --raw
  --store "local?root=$work/root" --plugin-files "$plugin")

opens() { # opens TRACE: O_RDWR opens of an index
  grep -c 'index/data\.mdb", O_RDWR' "$1" || true
}

aliased=$work/alias
XDG_CACHE_HOME=$aliased strace -f -e trace=openat -o "$work/alias.trace" \
  nix eval "${common[@]}" --eval-store "tarmac://$aliased/nix/tarmac/" \
  --expr "$(fetch "")" >/dev/null
n=$(opens "$work/alias.trace")
[[ $n == 1 ]] || {
  echo "FAIL: tarmac://<dir>/ and <dir>: one cache opened $n times in one process" >&2
  exit 1
}

linked=$work/linked
mkdir -p "$linked/nix" "$linked/real"
ln -s "$linked/real" "$linked/nix/tarmac"
XDG_CACHE_HOME=$linked nix eval "${common[@]}" --expr "$(fetch "")" >/dev/null
echo "one_cache.sh" >"$linked/real/poison"
XDG_CACHE_HOME=$linked strace -f -e trace=openat -o "$work/linked.trace" \
  nix eval "${common[@]}" --expr "$(fetch "") + $(fetch "?again=1")" >/dev/null
n=$(opens "$work/linked.trace")
[[ $n == 1 && -L $linked/nix/tarmac ]] || {
  echo "FAIL: a symlinked cache reset in one process: opened $n times; symlink kept: $([[ -L $linked/nix/tarmac ]] && echo yes || echo no)" >&2
  exit 1
}
echo "OK: one LMDB environment per cache in a process"
