# tests/c_api_after_reset.sh in the build sandbox: tests/query_hash_part.c
# built against the C API of the Nix the default plugin is built for
{
  runCommandCC,
  pkg-config,
  nix,
  plugin,
}:
runCommandCC "nix-tarmac-c-api-after-reset"
  {
    nativeBuildInputs = [
      nix
      pkg-config
    ];
    buildInputs = with nix.libs; [
      nix-main-c
      nix-store-c
      nix-util-c
    ];
  }
  ''
    export HOME=$TMPDIR
    $CC -Wall -Werror -o query_hash_part ${./query_hash_part.c} \
      $(pkg-config --cflags --libs nix-main-c nix-store-c nix-util-c)
    bash ${./c_api_after_reset.sh} ${plugin}/lib/nix/plugins "$PWD/query_hash_part" "$TMPDIR/work"
    touch $out
  ''
