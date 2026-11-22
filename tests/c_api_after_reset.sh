#!/usr/bin/env bash
# c_api_after_reset.sh PLUGIN_DIR QUERY WORK: a C API client that keeps a
# tarmac store open, as a daemon does, while its cache is replaced, refuses
# to say from the replaced cache which path a hash part names: no later
# process finds that path. Having taken that path from the cache before,
# it fails at exit, with 1 or its own failure's status, its output
# flushed, and within a bound. QUERY is tests/query_hash_part.c.
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

# three clients: one ends exiting 0, one exiting 7, as a failed build
# does, one exiting 256, which exit(3) truncates to 0
clients=(ok failing wraps)
declare -A pid fd exit_with=([ok]=0 [failing]=7 [wraps]=256)
for c in "${clients[@]}"; do
  mkfifo "$work/$c.in"
  NIX_CONFIG="plugin-files = $plugin" "$query" "$store" "${exit_with[$c]}" \
    <"$work/$c.in" >"$work/$c.out" 2>"$work/$c.err" &
  pid[$c]=$!
done
for c in "${clients[@]}"; do
  exec {f}>"$work/$c.in"
  fd[$c]=$f
done

# whether PID exits within its bound, in ticks like tests/watchdog.cpp
exits() {
  for _ in $(seq 1200); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done
  ! kill -0 "$1" 2>/dev/null
}

# client C's answer to its Nth query, bounded in ticks like tests/watchdog.cpp
answer() {
  echo "$hash" >&"${fd[$1]}"
  for _ in $(seq 600); do
    [[ $(wc -l <"$work/$1.out") -ge $2 ]] && break
    kill -0 "${pid[$1]}" 2>/dev/null || break
    sleep 0.1
  done
  sed -n "${2}p" "$work/$1.out"
}

for c in "${clients[@]}"; do
  first=$(answer "$c" 1)
  [[ $first == "path $name" ]] || {
    echo "FAIL: before the replacement, client $c answered '$first', not 'path $name'" >&2
    cat "$work/$c.err" >&2
    exit 1
  }
done

mv "$cache" "$cache.moved"
nix eval --raw "${common[@]}" --expr '"a new cache"' >/dev/null

failed=0
for c in "${clients[@]}"; do
  second=$(answer "$c" 2)
  exec {fd[$c]}>&-
  if ! exits "${pid[$c]}"; then
    kill -9 "${pid[$c]}"
    echo "FAIL: client $c did not exit:" >&2
    cat "$work/$c.err" >&2
    failed=1
    continue
  fi
  status=0
  wait "${pid[$c]}" || status=$?
  if [[ $second == "path $name" ]]; then
    echo "FAIL: after the replacement, client $c answered $name from the replaced cache, which no later process finds" >&2
    failed=1
    continue
  fi
  [[ $second == error*"was reset after this process opened it"* ]] || {
    echo "FAIL: after the replacement, client $c answered '$second', not the replaced cache's refusal" >&2
    cat "$work/$c.err" >&2
    failed=1
    continue
  }
  # it took $name from the cache before the replacement: it fails at exit,
  # keeping a failure's own status, and what it wrote reaches its output
  want=$((exit_with[$c] % 256 == 0 ? 1 : exit_with[$c] % 256))
  if [[ $status != "$want" ]] || ! grep -q "was reset after this process opened it" "$work/$c.err"; then
    echo "FAIL: client $c, which took $name from a cache since replaced, exited $status, not $want with ReplacedError:" >&2
    cat "$work/$c.err" >&2
    failed=1
  elif [[ $(tail -n 1 "$work/$c.out") != done ]]; then
    echo "FAIL: client $c's last line, left to exit(3) to flush, is lost:" >&2
    cat "$work/$c.out" >&2
    failed=1
  fi
done
[[ $failed == 0 ]]
echo "OK: a C API client refuses a hash part's path from a replaced cache, and fails at exit"
