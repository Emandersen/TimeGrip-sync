{
  description = "Timegrip → Google Calendar sync (C++ CLI)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }: let
    system = "x86_64-linux";
    pkgs   = nixpkgs.legacyPackages.${system};
  in {
    packages.${system} = {
      default = pkgs.stdenv.mkDerivation {
        pname   = "timegrip-sync";
        version = "1.0.0";
        src     = ./.;

        nativeBuildInputs = with pkgs; [ cmake pkg-config ];
        buildInputs = with pkgs; [
          curl
          nlohmann_json
          mariadb-connector-c
          openssl
        ];

        cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];

        installPhase = ''
          cmake --install . --prefix $out
        '';
      };

      fetch-schedule = let
        python = pkgs.python3.withPackages (ps: [
          ps.requests ps.browser-cookie3 ps.python-dotenv
        ]);
      in pkgs.writeShellApplication {
        name          = "fetch-schedule";
        runtimeInputs = [ python ];
        text          = "exec python3 ${./fetch_schedule.py} \"$@\"";
      };
    };

    apps.${system}.default = {
      type    = "app";
      program = "${self.packages.${system}.default}/bin/timegrip-sync";
    };

    devShells.${system}.default = pkgs.mkShell {
      packages = with pkgs; [
        cmake pkg-config
        curl nlohmann_json mariadb-connector-c openssl
        # Python dev tools
        (python3.withPackages (ps: [
          ps.requests ps.browser-cookie3 ps.python-dotenv
          ps.google-api-python-client ps.google-auth-oauthlib
          ps.google-auth-httplib2
        ]))
      ];
    };
  };
}
