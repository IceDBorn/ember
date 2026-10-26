{
  description = "ember: performance fixes for Elden Ring Nightreign";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      ember = import ./package.nix {
        inherit pkgs;
        src = ./.;
      };
      toolchain = [
        pkgs.pkgsCross.mingwW64.buildPackages.gcc
        pkgs.gnumake
      ];
      # Builds ember.dll from the working tree in the current directory.
      build = pkgs.writeShellApplication {
        name = "ember-build";
        runtimeInputs = toolchain;
        text = ''
          if [ ! -f Makefile ] || [ ! -d src ]; then
            echo "Run this from the ember source folder." >&2
            exit 1
          fi
          # The bare MinGW wrapper lacks the mcfgthread link path that stdenv adds.
          make CC="x86_64-w64-mingw32-gcc -L${pkgs.pkgsCross.mingwW64.windows.mcfgthreads}/lib" "$@"
        '';
      };
    in
    {
      packages.${system} = {
        inherit ember;
        default = ember;
      };

      apps.${system}.default = {
        type = "app";
        program = "${build}/bin/ember-build";
      };

      # `make` inside builds ember.dll with the MinGW cross compiler.
      devShells.${system}.default = pkgs.mkShell { packages = toolchain; };
    };
}
