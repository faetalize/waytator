{
  description = "Waytator is a screenshot annotator and lightweight image editor";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";

  outputs =
    { nixpkgs, ... }:
    let
      forEachSystem =
        fn:
        nixpkgs.lib.genAttrs [
          "aarch64-darwin"
          "aarch64-linux"
          "x86_64-darwin"
          "x86_64-linux"
        ] (system: fn system nixpkgs.legacyPackages.${system});
      version = "v1.0.0";
    in
    {
      packages = forEachSystem (
        system: pkgs: {
          default = pkgs.stdenv.mkDerivation {
            pname = "waytator";
            inherit version;
            nativeBuildInputs = with pkgs; [
              meson
              ninja
              pkg-config
              gtk4
              libadwaita
              gcc
              tesseract
            ];
            buildInputs = with pkgs; [
              tesseract
            ];
            src = pkgs.fetchFromGitHub {
              owner = "faetalize";
              repo = "waytator";
              tag = version;
              hash = "sha256-dRm5fDX/a4QSfV+rljC4WEgKpf6LZvyToeR6A/+coO8=";
            };

            buildPhase = ''
              runHook preBuild

              cd "$src"

              meson setup "$out/build" --buildtype=release
              meson compile -C "$out/build"

              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall

              install -D "$out/build/src/waytator" "$out/bin/waytator"
              rm -rf "$out/build"

              runHook postInstall
            '';
          };
        }
      );
      devShells = forEachSystem (
        system: pkgs: {
          default = pkgs.mkShell {
            packages = with pkgs; [
              meson
              ninja
              pkg-config
              gtk4
              libadwaita
              gcc
              tesseract
            ];
          };
        }
      );
    };
}
