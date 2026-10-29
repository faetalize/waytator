{
  description = "Screenshot annotator and lightweight image editor";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;

      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # taken from project() in meson.build so a release only bumps it there
      version =
        let
          lines = lib.splitString "\n" (builtins.readFile ./meson.build);
          matches = map (builtins.match "[[:space:]]*version: '([^']+)',?") lines;
        in
        lib.head (lib.findFirst (match: match != null) (throw "no version in meson.build") matches);

      mkWaytator =
        pkgs:
        pkgs.callPackage ./nix/package.nix {
          src = self;
          inherit version;
          commitTime = self.lastModified or 0;
        };
    in
    {
      packages = forAllSystems (pkgs: {
        waytator = mkWaytator pkgs;
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.waytator;
      });

      overlays.default = final: _prev: { waytator = mkWaytator final; };
    };
}
