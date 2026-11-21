# tests/after_reset.sh in the build sandbox, against the default plugin and
# the Nix it is built for
{
  runCommand,
  nix,
  plugin,
}:
runCommand "nix-tarmac-after-reset" { nativeBuildInputs = [ nix ]; } ''
  export HOME=$TMPDIR
  bash ${./after_reset.sh} ${plugin}/lib/nix/plugins "$TMPDIR/work"
  touch $out
''
