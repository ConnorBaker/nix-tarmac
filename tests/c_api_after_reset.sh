#!/usr/bin/env bash
# c_api_after_reset.sh PLUGIN_DIR QUERY WORK: a C API client that keeps a
# tarmac store open, as a daemon does, while its cache is replaced, refuses
# to say from the replaced cache which path a hash part names: no later
# process finds that path. QUERY is tests/query_hash_part.c.
#
# The cache is moved aside and another process opens a new one at its path,
# as a user's mv or rm and the next run do, or a reset by a process that
# does not wait for this one. Offline.
set -euo pipefail
plugin=$1
query=$2
work=$3
mkdir -p "$work"
export XDG_CACHE_HOME=$work/cache
cache=$XDG_CACHE_HOME/nix/tarmac
store=tarmac://$cache
common=(--extra-experimental-features nix-command
  --store "local?root=$work/root" --plugin-files "$plugin" --eval-store "$store")

# a derivation that another process wrote, before the replacement
drv=$(nix eval --raw "${common[@]}" --expr \
  '(derivation { name = "hash-part"; system = "x"; builder = "/bin/sh"; }).drvPath')
base=${drv##*/}
hash=${base%%-*}
# nix_store_path_name: the name after the hash part
name=${base#*-}

mkfifo "$work/in"
NIX_CONFIG="plugin-files = $plugin" "$query" "$store" \
  <"$work/in" >"$work/out" 2>"$work/err" &
pid=$!
exec {fd}>"$work/in"

# the answer to the Nth query, bounded in ticks like tests/watchdog.cpp
answer() {
  echo "$hash" >&"$fd"
  for _ in $(seq 600); do
    [[ $(wc -l <"$work/out") -ge $1 ]] && break
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  sed -n "${1}p" "$work/out"
}

first=$(answer 1)
[[ $first == "path $name" ]] || {
  echo "FAIL: before the replacement, the client answered '$first', not 'path $name'" >&2
  cat "$work/err" >&2
  exit 1
}

mv "$cache" "$cache.moved"
nix eval --raw "${common[@]}" --expr '"a new cache"' >/dev/null

second=$(answer 2)
exec {fd}>&-
wait "$pid" || true
if [[ $second == "path $name" ]]; then
  echo "FAIL: after the replacement, the client answered $name from the replaced cache, which no later process finds" >&2
  exit 1
fi
[[ $second == error*"was reset after this process opened it"* ]] || {
  echo "FAIL: after the replacement, the client answered '$second', not the replaced cache's refusal" >&2
  cat "$work/err" >&2
  exit 1
}
echo "OK: a C API client refuses a hash part's path from a replaced cache"
