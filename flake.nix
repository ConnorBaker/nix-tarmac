{
  description = "Fast tarball fetcher cache plugin for Nix";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
      scopeFor = system: nixpkgs.legacyPackages.${system}.callPackage ./packages.nix { };
      tarmacModule =
        {
          pkgs,
          lib,
          config,
          ...
        }:
        {
          options.nix-tarmac.package = lib.mkOption {
            type = lib.types.package;
            default =
              let
                scope = pkgs.callPackage ./packages.nix { };
              in
              scope.mkDispatcher (
                lib.optional (config.nix.package ? libs.nix-fetchers) (
                  pkgs.callPackage ./package.nix {
                    inherit (config.nix.package.libs) nix-fetchers nix-store nix-util;
                  }
                )
              );
            defaultText = "plugin built against config.nix.package";
          };
          config.nix.settings.plugin-files = [
            "${config.nix-tarmac.package}/lib/nix/plugins/nix-tarmac-loader${pkgs.stdenv.hostPlatform.extensions.sharedLibrary}"
          ];
        };
    in
    {
      packages = forAllSystems (
        system:
        let
          scope = scopeFor system;
        in
        {
          inherit (scope) default plugin-dispatcher;
        }
        // scope.versionPlugins
      );

      herculesCI = import ./effects.nix { inherit nixpkgs; };

      nixosModules.default = tarmacModule;
      darwinModules.default = tarmacModule;

      checks = forAllSystems (
        system:
        let
          scope = scopeFor system;
        in
        {
          inherit (scope) tests;
          inherit (scope) plugin-dispatcher;
        }
        // nixpkgs.lib.optionalAttrs (nixpkgs.lib.hasSuffix "-linux" system) {
          # Linux: strace, and the sandbox these scripts were run in
          inherit (scope) one-cache after-reset;

          # The tests as the deploy builds them: nix-optimization's tarmac.nix
          # builds Tarmac with llvmPackages_22.stdenv (to match its Clang-built
          # Nix libraries) and runs these tests. This pin mirrors that one by
          # hand; nothing keeps the two in step. glibc's fortified headers
          # route calls differently for Clang than for GCC (syslog() to
          # __vsyslog_chk instead of __syslog_chk, <bits/syslog.h>), which the
          # GCC-built tests above never exercise. glibc only: on Darwin there
          # is no such split, and crash_test's open_errors_keep_cache is
          # `#ifdef __linux__`.
          tests-clang = scope.tests.override {
            inherit (nixpkgs.legacyPackages.${system}.llvmPackages_22) stdenv;
          };
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ (scopeFor system).default ];
            packages =
              with pkgs;
              [
                clang-tools
                llvmPackages_latest.clang
              ]
              ++ lib.optionals stdenv.hostPlatform.isLinux [ perf ];
          };
        }
      );
    };
}
