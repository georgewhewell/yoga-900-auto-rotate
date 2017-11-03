{ pkgs ? (import <nixpkgs> { })}:

pkgs.stdenv.mkDerivation {
        name = "auto-rotate";
        version = "auto-rotate";

        src = pkgs.fetchFromGitHub {
          owner = "mrquincle";
          repo = "yoga-900-auto-rotate";
          rev = "master";
          sha256 = "0ibg4mxkdhf19pn7h4z9xjx6gh2i9r67869yz60bwzzfzybhyqj2";
        };

        buildInputs = with pkgs; [
          iio-sensor-proxy
          pkgconfig
          glib
          x11
          xorg.libXrandr
          xorg.libXi
        ];

        buildPhase = ''
          make
        '';

        installPhase = ''
          mkdir -p $out/bin
          cp auto-rotate $out/bin/
       '';
}
