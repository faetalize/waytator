# everything the spike runs, from the nixpkgs revision flake.lock pins, so CI and local machines match
let
  locked = (builtins.fromJSON (builtins.readFile ../../flake.lock)).nodes.nixpkgs.locked;
  pkgs = import (fetchTarball {
    url = "https://github.com/${locked.owner}/${locked.repo}/archive/${locked.rev}.tar.gz";
    sha256 = locked.narHash;
  }) { };

  probe = pkgs.runCommandCC "simtouch-probe"
    {
      nativeBuildInputs = [ pkgs.pkg-config ];
      buildInputs = [ pkgs.gtk4 ];
    }
    ''
      mkdir -p $out/bin
      $CC -Wall -o $out/bin/simtouch-probe ${./probe.c} $(pkg-config --cflags --libs gtk4)
    '';

  seatshim = pkgs.runCommandCC "simtouch-seatshim" { } ''
    mkdir -p $out/lib
    $CC -Wall -shared -fPIC -o $out/lib/simtouch-seatshim.so ${./seatshim.c} -ldl
  '';
in
pkgs.symlinkJoin {
  name = "simtouch-env";
  paths = [
    probe
    seatshim
    # unwrapped, the wrapper would start a D-Bus session the tests have no use for
    pkgs.sway-unwrapped
    pkgs.libinput.bin
    (pkgs.python3.withPackages (ps: [ ps.evdev ]))
  ];
}
