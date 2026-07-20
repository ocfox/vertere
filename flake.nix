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
        in
        {
          packages.default = pkgs.callPackage ./nix/package.nix { };

          devShells.default = pkgs.mkShell {
            # Tools that run on the build machine.
            nativeBuildInputs = [
              rustToolchain
              pkgs.pkg-config
              pkgs.wrapGAppsHook4
            ];

            # Libraries linked against. gtk4-layer-shell pulls the layer-shell
            # protocol bindings; the rest are gtk4's own pkg-config deps, which
            # the gtk4-rs build script resolves individually.
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

            # Invoked as subprocesses by src/capture.rs, so they are needed to run, not
            # to link.
            packages = [
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
        vertere = final.callPackage ./nix/package.nix { };
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

            # No option for the model or the languages: those are edited in the
            # settings window and kept in the database. Generating them here
            # would put the declarative copy in the read-only Nix store, where
            # the window could not write, leaving two sources of truth.

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
              # Unconditional: a resident daemon is the whole point of having one,
              # since the commands are bound to keys and the bubble should appear
              # at once rather than after a cold start.
              wantedBy = [ "graphical-session.target" ];

              serviceConfig = {
                # Type=dbus makes systemd wait for the name to appear on the bus,
                # so a command fired right after start cannot race registration.
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
