#!/usr/bin/env bash
# after_reset.sh PLUGIN_DIR WORK: processes that have the eval store open
# while another process resets its cache. A reset never waits for them,
# and none of them exits 0 having printed a path that no later process
# finds. Offline. Each part reports its failures and the next part runs.
#
# 1. A nix whose evaluation runs another nix that resets the cache
#    finishes, and fails.
# 2. Four `nix repl`s, each fed one line at a time, open the cache;
#    another process resets it, while they are idle; then each evaluates
#    one thing: a derivation and a file that another process wrote before
#    the reset, so valid only in the replaced cache; a derivation the repl
#    itself wrote before the reset, again; and a new derivation. Each must
#    fail with ReplacedError rather than print a path that no later
#    process finds.
# 3. Four repls take a path from the cache, one each way the eval store
#    hands one out: by writing a derivation (writeDerivation), by writing
#    a file (storeObject), by finding a derivation another process wrote
#    (drvBlob), and by finding a file another process wrote (pathRecord).
#    Another process resets the cache, while they are idle; then each
#    evaluates something that needs no store and exits. Each must exit
#    nonzero with ReplacedError.
set -euo pipefail
plugin=$1
work=$2
mkdir -p "$work"
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
# a fresh cache for each part
use_cache() {
  export XDG_CACHE_HOME=$work/$1
  store=tarmac://$XDG_CACHE_HOME/nix/tarmac
  common=(--extra-experimental-features 'nix-command flakes'
    --store "local?root=$work/root" --plugin-files "$plugin" --eval-store "$store")
}
value() { nix eval --raw "${common[@]}" --file "$work/lib.nix" --apply "lib: $1"; }
# whether PID exits within its bound, in ticks like tests/watchdog.cpp
exits() {
  for _ in $(seq 1200); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done
  ! kill -0 "$1" 2>/dev/null
}
declare -A pids fds seen printed
failed=0
# poisons the cache and resets it from another process, without this
# shell's write ends of the repls' inputs: 0 if the reset finished and
# succeeded within its bound. LABEL names its log
reset_cache() {
  echo after_reset.sh >"$XDG_CACHE_HOME/nix/tarmac/poison"
  (
    for name in "${!fds[@]}"; do exec {fds[$name]}>&-; done
    exec nix eval --raw "${common[@]}" --expr '"reset"'
  ) >/dev/null 2>"$work/$1.reset.err" &
  local reset=$!
  if ! exits "$reset"; then
    echo "FAIL: $1: a reset waited for idle processes that have the cache open:" >&2
    cat "$work/$1.reset.err" >&2
    kill -9 "$reset"
    return 1
  fi
  wait "$reset" || {
    echo "FAIL: $1: the reset failed:" >&2
    cat "$work/$1.reset.err" >&2
    exit 1
  }
}
# starts a repl per name, all before this shell opens any write end, so
# none holds another's: each sees end-of-file when its own write end closes
start_repls() {
  for name in "$@"; do
    mkfifo "$work/$name.in"
    nix repl "${common[@]}" --file "$work/lib.nix" \
      <"$work/$name.in" >"$work/$name.out" 2>"$work/$name.err" &
    pids[$name]=$!
  done
  for name in "$@"; do
    exec {fd}>"$work/$name.in"
    fds[$name]=$fd
  done
}
# sends LINE to repl NAME and waits for an answer matching PATTERN
answer() {
  echo "$2" >&"${fds[$1]}"
  for _ in $(seq 600); do grep -q -- "$3" "$work/$1.out" && return 0; sleep 0.1; done
  echo "FAIL: repl $1 never answered $2" >&2
  cat "$work/$1.out" "$work/$1.err" >&2
  exit 1
}
stop_repls() {
  for name in "$@"; do
    kill -9 "${pids[$name]}" 2>/dev/null || true
    exec {fds[$name]}>&-
    unset "fds[$name]"
  done
}

# 1. a nix whose evaluation runs another nix that resets the cache
use_cache cache-child
cat >"$work/child.sh" <<SH
echo after_reset.sh >"$XDG_CACHE_HOME/nix/tarmac/poison"
exec nix eval ${common[*]@Q} --expr '"child"'
SH
nix eval --raw "${common[@]}" --option allow-unsafe-native-code-during-evaluation true \
  --file "$work/lib.nix" --apply "lib: builtins.seq (lib.mk \"parent\").drvPath (builtins.exec [ \"bash\" \"$work/child.sh\" ])" \
  >"$work/parent.out" 2>"$work/parent.err" &
parent=$!
if ! exits "$parent"; then
  echo "FAIL: a nix and the nix its evaluation runs, on one cache, never finished:" >&2
  cat "$work/parent.err" >&2
  kill -9 "$parent"
  failed=1
else
  status=0
  wait "$parent" || status=$?
  if ! grep -q "resetting the cache" "$work/parent.err"; then
    echo "FAIL: the child did not reset the cache:" >&2
    cat "$work/parent.err" >&2
    failed=1
  elif [[ $status == 0 ]] || ! grep -q "was reset after this process opened it" "$work/parent.err"; then
    echo "FAIL: the parent, whose cache its child reset, exited $status:" >&2
    cat "$work/parent.out" "$work/parent.err" >&2
    failed=1
  else
    echo "OK: a nix running a nix that resets their cache finishes, and fails"
  fi
fi

# 2. repls that use the store after the reset
use_cache cache
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
start_repls "${!line[@]}"
for name in "${!line[@]}"; do
  answer "$name" "(mk \"${first[$name]}\").drvPath" "-${first[$name]}.drv\""
  seen[$name]=$(stat -c %s "$work/$name.out")
done
if ! reset_cache repls; then
  stop_repls "${!line[@]}"
  failed=1
else
  for name in "${!line[@]}"; do
    echo "${line[$name]}" >&"${fds[$name]}"
    exec {fds[$name]}>&-
    unset "fds[$name]"
    if ! exits "${pids[$name]}"; then
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
  echo "OK: evaluations spanning a reset fail"
fi

# 3. repls that took a path and do not use the store after the reset
use_cache cache-exit
declare -A took=(
  [wrote]='(mk "wrote").drvPath'
  [wrotefile]='file "wrotefile"'
  [found]='(mk "found").drvPath'
  [foundfile]='file "foundfile"'
)
found=$(value '(lib.mk "found").drvPath')
foundfile=$(value 'lib.file "foundfile"')
start_repls "${!took[@]}"
for name in "${!took[@]}"; do
  answer "$name" "${took[$name]}" "-$name\(\.drv\)\?\""
  printed[$name]=$(grep -o -E -- "/nix/store/[a-z0-9]{32}-$name(\.drv)?" "$work/$name.out")
done
[[ ${printed[found]} == "$found" && ${printed[foundfile]} == "$foundfile" ]]
if ! reset_cache exit; then
  stop_repls "${!took[@]}"
  failed=1
else
  for name in "${!took[@]}"; do
    echo '"needs no " + "store"' >&"${fds[$name]}"
    exec {fds[$name]}>&-
    unset "fds[$name]"
    status=0
    if ! exits "${pids[$name]}"; then
      kill -9 "${pids[$name]}"
      echo "FAIL: after the reset, repl $name did not exit" >&2
      failed=1
      continue
    fi
    wait "${pids[$name]}" || status=$?
    if [[ $status == 0 ]]; then
      valid=valid
      nix path-info "${common[@]}" --store "$store" "${printed[$name]}" >/dev/null 2>&1 || valid=invalid
      echo "FAIL: repl $name printed ${printed[$name]} and exited 0 after a reset; ${printed[$name]} is $valid" >&2
      failed=1
    elif ! grep -q "was reset after this process opened it" "$work/$name.err"; then
      echo "FAIL: repl $name exited $status for another reason:" >&2
      cat "$work/$name.err" >&2
      failed=1
    else
      echo "OK: repl $name, which took a path from a cache since reset, fails at exit"
    fi
  done
fi
[[ $failed == 0 ]]
