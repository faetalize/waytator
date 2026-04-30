{
  lib,
  stdenv,
  version,
  meson,
  ninja,
  pkg-config,
  gtk4,
  libadwaita,
  tesseract,
  makeWrapper,
}:
stdenv.mkDerivation {
  pname = "waytator";
  inherit version;

  src = lib.cleanSource ../.;

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    makeWrapper
  ];

  buildInputs = [
    gtk4
    libadwaita
    tesseract
  ];

  postFixup = ''
    wrapProgram $out/bin/waytator \
      --prefix PATH : ${lib.makeBinPath [ tesseract ]}
  '';

  mesonBuildType = "release";

  meta = {
    homepage = "https://github.com/faetalize/waytator";
    description = "A screenshot annotator and lightweight image editor";
    license = lib.licenses.gpl3Plus;
    platforms = lib.platforms.linux;
    mainProgram = "waytator";
  };
}
