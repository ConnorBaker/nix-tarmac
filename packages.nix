# One plugin per supported Nix version plus a dispatcher bundle.
{
  lib,
  newScope,
  fetchFromGitHub,
  nixVersions,
  re2c,
  symlinkJoin,
}:
lib.makeScope newScope (
  self:
  let
    # nixpkgs' nixVersions.git lags behind master. Track it closer so API
    # breakage shows up here before downstream flakes pull a newer nix.
    # Bumped weekly by the update-nix-git effect (effects.nix).
    nixGitPin = lib.importJSON ./nix-git.json;
    nixGitSrc = fetchFromGitHub {
      inherit (nixGitPin)
        owner
        repo
        rev
        hash
        ;
    };
    nixGit =
      ((nixVersions.nixComponents_git.overrideSource nixGitSrc).overrideScope (
        _final: prev: {
          inherit (nixGitPin) version;
          # Nix 15e1eb4b54 (2.37pre) lexes with re2c instead of flex; the
          # pinned nixpkgs' nix-expr recipe gives it only flex. re2c is
          # added beside flex, so nix-expr configures whether nix-git.json
          # is before that commit (flex) or at or after it (re2c). nixpkgs
          # dfd52640f0b8 picks one by version instead: drop this override
          # once the nixpkgs input has that.
          nix-expr = prev.nix-expr.overrideAttrs (old: {
            nativeBuildInputs = old.nativeBuildInputs ++ [ re2c ];
          });
        }
      )).nix-everything;
    nixFor = version: if version == "git" then nixGit else nixVersions.${version};

    supportedNixVersions = builtins.filter (
      name:
      builtins.match "nix_[0-9]+_[0-9]+" name != null
      && (builtins.tryEval (
        lib.versionAtLeast nixVersions.${name}.version "2.34"
        && (nixVersions.${name}.libs or { }) ? nix-fetchers
      )).value or false
    ) (builtins.attrNames nixVersions);
  in
  {
    tests = self.callPackage ./package.nix { plugin = false; };

    default = self.callPackage ./package.nix {
      inherit (nixVersions.latest.libs) nix-fetchers nix-store nix-util;
    };

    versionPlugins = lib.listToAttrs (
      map (
        version:
        lib.nameValuePair "plugin-${version}" (
          (self.callPackage ./package.nix {
            inherit ((nixFor version).libs) nix-fetchers nix-store nix-util;
          }).overrideAttrs
            (old: {
              # tests/e2e.sh runs the plugin under exactly this nix
              passthru = (old.passthru or { }) // {
                nix = nixFor version;
              };
            })
        )
      ) (supportedNixVersions ++ [ "git" ])
    );

    # The loader dlopen()s the build matching the running Nix version.
    # extraPlugins come first so exact-match builds win over nixpkgs ones.
    mkDispatcher =
      extraPlugins:
      symlinkJoin {
        name = "nix-tarmac-dispatcher";
        paths =
          extraPlugins
          ++ [ self.default ]
          ++ map (version: self.versionPlugins."plugin-${version}") (supportedNixVersions ++ [ "git" ]);
        # dladdr() resolves symlinks, so the loader must be a real file here
        postBuild = ''
          rm -f "$out"/lib/nix/plugins/nix-tarmac-loader.*
          cp -L ${self.default}/lib/nix/plugins/nix-tarmac-loader.* "$out"/lib/nix/plugins/
        '';
      };

    plugin-dispatcher = self.mkDispatcher [ ];
  }
)
