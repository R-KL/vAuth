# flake.nix
{
  description = "vAuth - TPM-backed virtual FIDO2 authenticator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems =
        f:
        nixpkgs.lib.genAttrs systems (
          system:
          let
            pkgs = import nixpkgs { inherit system; };

            # ------------------------------------------------------------
            # Slint C++ SDK (prebuilt binary release)
            # ------------------------------------------------------------
            slint = pkgs.stdenvNoCC.mkDerivation {
              pname = "slint-cpp";
              version = "1.18.1";

              src = pkgs.fetchurl {
                url = "https://github.com/slint-ui/slint/releases/download/v1.18.1/Slint-cpp-1.18.1-Linux-${
                  pkgs.lib.optionalString (system == "x86_64-linux") "x86_64"
                }${pkgs.lib.optionalString (system == "aarch64-linux") "arm64"}.tar.gz";

                hash = "${
                  pkgs.lib.optionalString (
                    system == "x86_64-linux"
                  ) "sha256-8LJOpgG4Cv4kFnfRF5v1IV3OrlKfKydbZ4oK+aFEslU="
                }${
                  pkgs.lib.optionalString (
                    system == "aarch64-linux"
                  ) "sha256-gQz+14bD9jrqnBtzNxoC7VkRKdGOB3CUr6ngJOLXFP0="
                }";
              };

              nativeBuildInputs = [
                pkgs.autoPatchelfHook
              ];

              buildInputs = with pkgs; [
                fontconfig
                freetype
                libGL
                libxkbcommon
                wayland
                systemd
                libinput
                mesa
              ];

              dontConfigure = true;
              dontBuild = true;

              installPhase = ''
                mkdir -p "$out"
                cp -a . "$out/"
              '';
            };

            # ------------------------------------------------------------
            # Nix-side CMake compatibility package for rlottie
            #
            # vAuth's upstream CMake uses:
            #   find_package(rlottie REQUIRED)
            #
            # nixpkgs provides rlottie through pkg-config, so provide
            # the CMake package config without modifying vAuth's source.
            # ------------------------------------------------------------
            rlottieCmake = pkgs.writeTextDir "lib/cmake/rlottie/rlottieConfig.cmake" ''
              find_package(PkgConfig REQUIRED)
              pkg_check_modules(RLOTTIE REQUIRED IMPORTED_TARGET rlottie)

              if(NOT TARGET rlottie::rlottie)
                add_library(rlottie::rlottie ALIAS PkgConfig::RLOTTIE)
              endif()

              set(rlottie_FOUND TRUE)
            '';

            # ------------------------------------------------------------
            # vAuth package
            # ------------------------------------------------------------
            vauth = pkgs.stdenv.mkDerivation {
              pname = "vauth";
              version = "0.1.0";
              src = self;

              nativeBuildInputs = with pkgs; [
                cmake
                ninja
                pkg-config
                makeWrapper
              ];

              buildInputs = with pkgs; [
                cli11
                nlohmann_json
                tinycbor
                openssl
                tpm2-tss
                json_c
                curl
                pam
                sdbus-cpp_2
                systemd
                rlottie
                wayland
                slint
              ];

              # Make both Slint and the Nix-provided rlottie CMake
              # compatibility package discoverable.
              preConfigure = ''
                export CMAKE_PREFIX_PATH="${slint}:${rlottieCmake}:''${CMAKE_PREFIX_PATH:-}"
              '';

              cmakeFlags = [
                "-DCMAKE_BUILD_TYPE=Release"
                "-DVAUTH_BUILD_UI=ON"
                "-DVAUTH_BUILD_DEBUG_AGENT=OFF"
                "-DVAUTH_LINK_SLINT_STATIC=OFF"

                # Runtime paths
                "-DVAUTH_FAPI_SYSTEM_DIR=/var/lib/tpm2-tss/keystore"
                "-DVAUTH_PAM_CONFIG_DIR=${placeholder "out"}/etc/vauth/config"

                # Nix package-relative installation paths
                "-DVAUTH_DBUS_SYSTEM_POLICY_DIR=${placeholder "out"}/share/dbus-1/system.d"
              ];

              postInstall = ''
                wrapProgram $out/bin/vauth-ui \
                  --set SLINT_BACKEND winit-skia-software \
                  --prefix LD_LIBRARY_PATH : ${
                    pkgs.lib.makeLibraryPath [
                      pkgs.wayland
                    ]
                  }
              '';
            };

            # Runtime libs for dev shell / app wrapper
            slintRuntimeLibs = with pkgs; [
              fontconfig
              freetype
              libGL
              libxkbcommon
              wayland
            ];
          in
          f {
            inherit
              pkgs
              slint
              rlottieCmake
              vauth
              slintRuntimeLibs
              ;
          }
        );

    in
    {
      packages = forAllSystems (
        {
          vauth,
          slint,
          ...
        }:
        {
          default = vauth;
          inherit vauth slint;
        }
      );

      devShells = forAllSystems (
        {
          pkgs,
          slint,
          rlottieCmake,
          slintRuntimeLibs,
          ...
        }:
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              gcc
              cmake
              ninja
              pkg-config
              git

              cli11
              nlohmann_json
              tinycbor
              openssl
              tpm2-tss
              tpm2-tools
              json_c
              pam
              sdbus-cpp_2
              systemd
              rlottie

              gdb
              valgrind

              slint
              wayland
              libxkbcommon
              libGL
              freetype
            ];

            shellHook = ''
              export CMAKE_PREFIX_PATH="${slint}:${rlottieCmake}:''${CMAKE_PREFIX_PATH:-}"

              export LD_LIBRARY_PATH="${
                pkgs.lib.makeLibraryPath ([ slint ] ++ slintRuntimeLibs)
              }:''${LD_LIBRARY_PATH:-}"

              echo "=== vAuth Development Environment ==="
              echo "Slint SDK: ${slint}"
              echo "rlottie CMake compatibility: ${rlottieCmake}"
              echo ""
              echo "Build with:"
              echo "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug"
              echo "  cmake --build build"
              echo ""
              echo "Test install to \$TMPDIR:"
              echo "  DESTDIR=\$TMPDIR/install cmake --install build"
            '';
          };
        }
      );

      apps = forAllSystems (
        {
          pkgs,
          vauth,
          slint,
          slintRuntimeLibs,
          ...
        }:
        {
          default = {
            type = "app";

            program =
              (pkgs.writeShellScriptBin "vauth-ui" ''
                export LD_LIBRARY_PATH="${
                  pkgs.lib.makeLibraryPath ([ slint ] ++ slintRuntimeLibs)
                }:''${LD_LIBRARY_PATH:-}"

                exec "${vauth}/bin/vauth-ui" "$@"
              '').outPath
              + "/bin/vauth-ui";

            meta.description = "vAuth graphical interface";
          };
        }
      );

      # --------------------------------------------------------------
      # NixOS module
      # --------------------------------------------------------------
      nixosModules.default =
        { pkgs, ... }:
        {
          imports = [ ./module.nix ];
          _module.args.vauth = self.packages.${pkgs.stdenv.hostPlatform.system}.vauth;
        };
    };
}
