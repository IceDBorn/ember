{
  pkgs,
  src,
}:

# Cross-compiles ember.dll with MinGW (imports only system DLLs).
pkgs.pkgsCross.mingwW64.stdenv.mkDerivation {
  name = "ember";
  inherit src;
  dontConfigure = true;
  buildPhase = ''
    make CC=$CC
  '';
  installPhase = ''
    mkdir -p $out
    cp ember.dll ember.ini $out/
  '';
}
