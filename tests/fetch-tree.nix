# tests/fetch_tree.sh in the build sandbox, against a plugin and the Nix it
# is built for
{
  runCommand,
  gnutar,
  gzip,
  nix,
  plugin,
}:
runCommand "nix-tarmac-fetch-tree"
  {
    nativeBuildInputs = [
      nix
      gnutar
      gzip
    ];
  }
  ''
    export HOME=$TMPDIR
    bash ${./fetch_tree.sh} ${plugin}/lib/nix/plugins "$TMPDIR/work"
    touch $out
  ''
