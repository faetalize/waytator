{
  description = "Fast screen capture and editing";

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

      # its own output rather than only an override, so it can be run straight from the flake
      mkWaytatorMinimal = pkgs: (mkWaytator pkgs).override { enableOcr = false; };
    in
    {
      packages = forAllSystems (pkgs: {
        waytator = mkWaytator pkgs;
        waytator-minimal = mkWaytatorMinimal pkgs;
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.waytator;
      });

      overlays.default = final: _prev: {
        waytator = mkWaytator final;
        waytator-minimal = mkWaytatorMinimal final;
      };
    };
}
