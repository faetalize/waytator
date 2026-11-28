# everything tests/e2e needs: waytator from this checkout with the test report built in, and the compositor,
# python and seat shim around it, from the nixpkgs flake.lock pins, so CI and every machine run the same
let
  pins = builtins.fromJSON (builtins.readFile ./pins.json);
  locked = (builtins.fromJSON (builtins.readFile ../../flake.lock)).nodes.nixpkgs.locked;
  pkgs = import
    (fetchTarball {
      url = "https://github.com/${locked.owner}/${locked.repo}/archive/${locked.rev}.tar.gz";
      sha256 = locked.narHash;
    })
    { config = { }; overlays = [ ]; };
  inherit (pkgs) lib;

  simtouch = import
    (fetchTarball {
      url = "https://github.com/${pins.simtouch.repo}/archive/${pins.simtouch.rev}.tar.gz";
      sha256 = pins.simtouch.hash;
    })
    { inherit pkgs; };

  root = toString ../..;

  waytator = (pkgs.callPackage ../../nix/package.nix {
    # the checkout as it is, without what the app isn't built from, so editing the tests doesn't rebuild it
    src = lib.cleanSourceWith {
      src = ../..;
      filter = path: type:
        let
          name = baseNameOf path;
        in
        path != "${root}/tests/e2e"
        && !(type == "directory" && (name == ".git" || lib.hasPrefix "build" name))
        && !(type == "symlink" && lib.hasPrefix "result" name);
    };
    version = "e2e";
    enableOcr = false;
  }).overrideAttrs (old: {
    mesonFlags = old.mesonFlags ++ [ "-Dtest_report=true" ];
    # the unit tests run with every ordinary build, see nix.yml and distros.yml
    doCheck = false;
    # the package puts wl-clipboard ahead on PATH, which would get past the tests' stand-in for wl-copy
    preFixup = "";
  });

  # unwrapped, the wrapper would start a D-Bus session of its own
  sway = pkgs.sway-unwrapped;
in
# the pin is only what a machine without Nix is compared with, so a newer sway here warns rather than fails
lib.warnIf (sway.version != pins.sway)
  "tests/e2e/pins.json says sway ${pins.sway}, flake.lock's nixpkgs has ${sway.version}"
  (pkgs.symlinkJoin {
    name = "waytator-e2e";
    paths = [
      waytator
      sway
      pkgs.dbus
      simtouch.seatshim
      (pkgs.python3.withPackages (ps: [ ps.evdev ps.pytest simtouch.simtouch ]))
    ];
  })
