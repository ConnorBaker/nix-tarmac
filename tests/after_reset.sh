#!/usr/bin/env bash
# after_reset.sh PLUGIN_DIR WORK: a process that opened the eval store
# before another process reset it, and uses the store after the reset,
# fails rather than answer from or write to the replaced cache. (What such
# a process wrote before the reset is lost with it; one that does not use
# the store again after the reset does not fail.)
#
# Four `nix repl`s, each fed one line at a time, open the cache; another
# process resets it; then each evaluates one thing: a derivation and a
# file that another process wrote before the reset, so valid only in the
# replaced cache; a derivation the repl itself wrote before the reset,
# again; and a new derivation. Each must fail with ReplacedError rather
# than print a path that no later process finds. Offline.
set -euo pipefail
plugin=$1
work=$2
mkdir -p "$work"
export XDG_CACHE_HOME=$work/cache
store=tarmac://$XDG_CACHE_HOME/nix/tarmac
common=(--extra-experimental-features 'nix-command flakes'
  --store "local?root=$work/root" --plugin-files "$plugin" --eval-store "$store")
cat >"$work/lib.nix" <<'NIX'
{
  mk = name: derivation {
    inherit name;
    system = "x";
    builder = "/bin/sh";
  };
  file = name: builtins.toFile name "written before the reset";
}
NIX
value() { nix eval --raw "${common[@]}" --file "$work/lib.nix" --apply "lib: $1"; }

# written before the reset, by another process
value '(lib.mk "opened").drvPath' >/dev/null
value '(lib.mk "early").drvPath' >/dev/null
value 'lib.file "early-file"' >/dev/null
# what each repl evaluates before the reset, and after it
declare -A first=(
  [early]=opened
  [early-file]=opened
  [rewrite]=rewrite
  [late]=opened
)
declare -A line=(
  [early]='(mk "early").drvPath'
  [early-file]='file "early-file"'
  [rewrite]='(mk "rewrite").drvPath'
  [late]='(mk "late").drvPath'
)
declare -A pids fds seen

# every repl starts before this shell opens any write end, so none holds
# another's: each sees end-of-file when its own write end closes
for name in "${!line[@]}"; do
  mkfifo "$work/$name.in"
  nix repl "${common[@]}" --file "$work/lib.nix" \
    <"$work/$name.in" >"$work/$name.out" 2>"$work/$name.err" &
  pids[$name]=$!
done
for name in "${!line[@]}"; do
  exec {fd}>"$work/$name.in"
  fds[$name]=$fd
  echo "(mk \"${first[$name]}\").drvPath" >&"$fd"
done
for name in "${!line[@]}"; do
  for _ in $(seq 600); do grep -q -- "-${first[$name]}.drv\"" "$work/$name.out" && break; sleep 0.1; done
  grep -q -- "-${first[$name]}.drv\"" "$work/$name.out" || {
    echo "FAIL: repl $name never answered" >&2
    cat "$work/$name.out" "$work/$name.err" >&2
    exit 1
  }
  seen[$name]=$(stat -c %s "$work/$name.out")
done

# another process resets the cache under the repls
echo after_reset.sh >"$XDG_CACHE_HOME/nix/tarmac/poison"
nix eval --raw "${common[@]}" --expr '"reset"' >/dev/null

failed=0
for name in "${!line[@]}"; do
  echo "${line[$name]}" >&"${fds[$name]}"
  exec {fds[$name]}>&-
  # bounded in ticks, like tests/watchdog.cpp: a repl that never exits
  # fails the check instead of holding the build
  for _ in $(seq 1200); do kill -0 "${pids[$name]}" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "${pids[$name]}" 2>/dev/null; then
    kill -9 "${pids[$name]}"
    echo "FAIL: after the reset, repl $name did not exit" >&2
    failed=1
    continue
  fi
  wait "${pids[$name]}" || true
  # what the repl printed after the reset; a path ends -<name>[.drv]
  tail -c +$((seen[$name] + 1)) "$work/$name.out" >"$work/$name.after"
  if grep -q -E -- "-$name(\.drv)?\"" "$work/$name.after"; then
    echo "FAIL: an evaluation spanning a reset succeeded: $name printed $(grep -o -E -- "[a-z0-9]{32}-$name(\.drv)?" "$work/$name.after"), which no later process finds" >&2
    failed=1
  elif ! grep -q "was reset after this process opened it" "$work/$name.err"; then
    echo "FAIL: after the reset, $name failed for another reason:" >&2
    cat "$work/$name.err" >&2
    failed=1
  fi
done
[[ $failed == 0 ]]
echo "OK: evaluations spanning a reset fail"
