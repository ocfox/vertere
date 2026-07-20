{
  lib,
  rustPlatform,
  pkg-config,
  wrapGAppsHook4,
  gtk4,
  gtk4-layer-shell,
  glib,
  cairo,
  pango,
  gdk-pixbuf,
  graphene,
  wayland,
  grim,
  slurp,
  wl-clipboard,
}:
let
  # Resolved at runtime through PATH rather than baked in, so this list is
  # the single place that decides which ones get used.
  runtimeTools = [
    grim
    slurp
    wl-clipboard
  ];
in
rustPlatform.buildRustPackage {
  pname = "vertere";
  version = (builtins.fromTOML (builtins.readFile ../Cargo.toml)).package.version;
  src = lib.cleanSource ../.;
  cargoLock.lockFile = ../Cargo.lock;

  nativeBuildInputs = [
    pkg-config
    wrapGAppsHook4
  ];

  buildInputs = [
    gtk4
    gtk4-layer-shell
    glib
    cairo
    pango
    gdk-pixbuf
    graphene
    wayland
  ];

  # No unit file here: the NixOS module below defines it natively, so
  # that ExecStart can pick up `--config` and the environment file.
  # Shipping a second copy would only give the two a chance to drift.
  postInstall = ''
    mkdir -p $out/share
    cp -r data/applications data/icons -t $out/share/
  '';

  preFixup = ''
    gappsWrapperArgs+=(
      --prefix PATH : ${lib.makeBinPath runtimeTools}
    )
  '';

  meta = {
    description = "Wayland translator";
    homepage = "https://github.com/ocfox/vertere";
    license = lib.licenses.mit;
    mainProgram = "vertere";
    platforms = lib.platforms.linux;
  };
}
