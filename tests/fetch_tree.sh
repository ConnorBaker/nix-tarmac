#!/usr/bin/env bash
# fetch_tree.sh PLUGIN_DIR WORK: a tarball's tree read through the plugin's
# SourceAccessor is the tree the builtin fetcher reads: the same narHash,
# the same store path once the tree is copied into a store, and the same
# listing, every directory read with builtins.readDir. Copying the tree and
# reading its directories go through the accessor's readDirectory; the
# narHash alone does not (the plugin computes it from its own tree). Read
# cold (just fetched) and warm (from the plugin's cache, with the tarball
# gone). Offline: the tarball is a file:// URL.
set -euo pipefail
plugin=$1
work=$2
mkdir -p "$work/src/sub/deeper" "$work/src/.hidden"
echo hello >"$work/src/file.txt"
echo world >"$work/src/sub/nested.txt"
echo deep >"$work/src/sub/deeper/leaf"
echo dots >"$work/src/sub/..."
echo space >"$work/src/sub/a b"
echo hidden >"$work/src/.hidden/x"
printf '#!/bin/sh\n' >"$work/src/run.sh"
chmod +x "$work/src/run.sh"
ln -s file.txt "$work/src/link"
mkdir "$work/src/empty"
tar -C "$work" -czf "$work/src.tar.gz" src

expr="let
  t = builtins.fetchTree { type = \"tarball\"; url = \"file://$work/src.tar.gz\"; };
  walk = p: builtins.mapAttrs
    (n: type: if type == \"directory\" then walk \"\${p}/\${n}\" else type)
    (builtins.readDir p);
in builtins.toJSON { inherit (t) narHash; path = t.outPath; tree = walk t.outPath; }"

# a fresh store and cache each: a store path already valid would not be
# copied again, and so not read through the accessor
read_tree() {
  local name=$1
  shift
  XDG_CACHE_HOME=$work/cache-$name nix eval --impure --raw \
    --extra-experimental-features 'nix-command flakes' \
    --store "local?root=$work/root-$name" "$@" --expr "$expr"
}

builtin=$(read_tree builtin)
# what is compared is the whole listing, not the store path alone (toJSON
# would make a set with an outPath attribute that path alone)
for want in '"narHash":"sha256-' '"run.sh":"regular"' '"link":"symlink"' '"empty":{}' '"leaf":"regular"'; do
  [[ $builtin == *"$want"* ]] || {
    echo "FAIL: the builtin fetcher's result lacks $want: $builtin" >&2
    exit 1
  }
done
plugin_cold=$(read_tree plugin --plugin-files "$plugin")
[[ -e $work/cache-plugin/nix/tarmac/pack.0 ]] || {
  echo "FAIL: the plugin did not fetch the tarball" >&2
  exit 1
}
mv "$work/src.tar.gz" "$work/src.tar.gz.hidden"
cp -r "$work/cache-plugin" "$work/cache-plugin-warm"
plugin_warm=$(read_tree plugin-warm --plugin-files "$plugin")

same() {
  [[ $2 == "$builtin" ]] || {
    echo "FAIL $1: the plugin's tree differs from the builtin fetcher's" >&2
    echo "builtin: $builtin" >&2
    echo "plugin:  $2" >&2
    exit 1
  }
}
same cold "$plugin_cold"
same warm "$plugin_warm"
echo "OK: $builtin"
