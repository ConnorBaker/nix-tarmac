# tests/fetch_error.sh in the build sandbox, against the default plugin and
# the Nix it is built for
{
  runCommand,
  gnutar,
  gzip,
  python3,
  nix,
  plugin,
}:
runCommand "nix-tarmac-fetch-error"
  {
    nativeBuildInputs = [
      nix
      gnutar
      gzip
      python3
    ];
  }
  ''
    export HOME=$TMPDIR
    bash ${./fetch_error.sh} ${plugin}/lib/nix/plugins "$TMPDIR/work"
    touch $out
  ''
