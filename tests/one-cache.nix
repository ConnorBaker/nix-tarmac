# tests/one_cache.sh in the build sandbox, against the default plugin and
# the Nix it is built for
{
  runCommand,
  strace,
  gnutar,
  gzip,
  nix,
  plugin,
}:
runCommand "nix-tarmac-one-cache"
  {
    nativeBuildInputs = [
      nix
      strace
      gnutar
      gzip
    ];
  }
  ''
    export HOME=$TMPDIR
    bash ${./one_cache.sh} ${plugin}/lib/nix/plugins "$TMPDIR/work"
    touch $out
  ''
