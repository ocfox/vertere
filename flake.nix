{
  description = "vertere — Wayland translator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    inputs@{ flake-parts, ... }:
    let
      mkVertere =
        pkgs:
        pkgs.rustPlatform.buildRustPackage {
          pname = "vertere";
          version = (builtins.fromTOML (builtins.readFile ./Cargo.toml)).package.version;
          src = pkgs.lib.cleanSource ./.;
          cargoLock.lockFile = ./Cargo.lock;

          nativeBuildInputs = [
            pkgs.pkg-config
            pkgs.wrapGAppsHook4
          ];

          buildInputs = [
            pkgs.gtk4
            pkgs.gtk4-layer-shell
            pkgs.glib
            pkgs.cairo
            pkgs.pango
            pkgs.gdk-pixbuf
            pkgs.graphene
            pkgs.wayland
          ];

          postInstall = ''
            mkdir -p $out/share
            cp -r data/applications data/icons -t $out/share/
          '';

          preFixup = ''
            gappsWrapperArgs+=(
              --prefix PATH : ${
                pkgs.lib.makeBinPath [
                  pkgs.grim
                  pkgs.slurp
                  pkgs.wl-clipboard
                ]
              }
            )
          '';

          meta = {
            description = "Wayland translator";
            homepage = "https://github.com/ocfox/vertere";
            license = pkgs.lib.licenses.mit;
            mainProgram = "vertere";
            platforms = pkgs.lib.platforms.linux;
          };
        };
    in
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      perSystem =
        { system, ... }:
        let
          pkgs = import inputs.nixpkgs {
            inherit system;
            overlays = [ inputs.rust-overlay.overlays.default ];
          };

          rustToolchain = pkgs.rust-bin.stable.latest.default.override {
            extensions = [
              "rust-src"
              "rust-analyzer"
              "clippy"
              "rustfmt"
            ];
          };

          vertere = mkVertere pkgs;
        in
        {
          packages.default = vertere;

          devShells.default = pkgs.mkShell {
            inputsFrom = [ vertere ];

            packages = [
              rustToolchain
              pkgs.grim
              pkgs.slurp
              pkgs.wl-clipboard
            ];

            env = {
              RUST_SRC_PATH = "${rustToolchain}/lib/rustlib/src/rust/library";
              RUST_BACKTRACE = "1";
            };
          };

          formatter = pkgs.nixfmt-tree;
        };

      flake.overlays.default = final: prev: {
        vertere = mkVertere final;
      };

      flake.nixosModules.default =
        {
          config,
          lib,
          pkgs,
          ...
        }:
        let
          cfg = config.services.vertere;
        in
        {
          options.services.vertere = {
            enable = lib.mkEnableOption "the Vertere translation daemon";

            package = lib.mkOption {
              type = lib.types.package;
              default = inputs.self.packages.${pkgs.stdenv.hostPlatform.system}.default;
              defaultText = lib.literalExpression "vertere.packages.\${system}.default";
              description = "The vertere package to use.";
            };

            environmentFile = lib.mkOption {
              type = lib.types.nullOr lib.types.path;
              default = null;
              example = "/run/secrets/vertere";
              description = ''
                File holding `API_KEY=...`, read at startup. Keep it out of the
                Nix store — point at a secret manager's output or a path with
                mode 0600.
              '';
            };
          };

          config = lib.mkIf cfg.enable {
            environment.systemPackages = [ cfg.package ];

            systemd.user.services.vertere = {
              description = "Vertere translation daemon";
              partOf = [ "graphical-session.target" ];
              after = [ "graphical-session.target" ];
              wantedBy = [ "graphical-session.target" ];

              serviceConfig = {
                Type = "dbus";
                BusName = "me.ocfox.Vertere";
                ExecStart = "${lib.getExe cfg.package} daemon";
                EnvironmentFile = lib.mkIf (cfg.environmentFile != null) cfg.environmentFile;
                Restart = "on-failure";
                RestartSec = 2;
              };
            };
          };
        };
    };
}
