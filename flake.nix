{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    systems.url = "github:nix-systems/default-linux";
  };

  outputs =
    {
      self,
      nixpkgs,
      systems,
      ...
    }:
    let
      eachSystem =
        fn: nixpkgs.lib.genAttrs (import systems) (system: fn nixpkgs.legacyPackages.${system});

      version = self.shortRev or "dirty";
    in
    {
      overlays.default = final: _prev: {
        waytator = final.callPackage ./nix/package.nix { inherit version; };
      };

      packages = eachSystem (pkgs: {
        waytator = pkgs.callPackage ./nix/package.nix { inherit version; };
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.waytator;
      });
    };
}
