{
  inputs.nixpkgs.url = "nixpkgs/nixos-23.11";

  outputs = {
    self,
    nixpkgs,
  }: let
    system = "x86_64-linux";
    pkgs = import nixpkgs {
      inherit system;
    };
  in {
    packages.x86_64-linux = rec {
      firmware = with pkgs;
        stdenv.mkDerivation rec {
          name = "kinect-firmware";

          src = fetchurl {
            url = "http://download.microsoft.com/download/F/9/9/F99791F2-D5BE-478A-B77A-830AD14950C3/KinectSDK-v1.0-beta2-x86.msi";
            hash = "sha256-gXdkWRz/esw9Z4xbxl3Icks9JDYRwQENqywY0N7dQiE=";
          };

          buildInputs = [p7zip];
          unpackPhase = ''
            7z e -y -r ${src} "UACFirmware.*" > /dev/null
          '';
          installPhase = ''
            FW_FILE=$(ls UACFirmware.* | cut -d ' ' -f 1)
            cat $FW_FILE > $out
          '';
        };

      upload = with pkgs;
        stdenv.mkDerivation {
          name = "kinect-upload-fw";
          nativeBuildInputs = [pkg-config];
          buildInputs = [libusb];
          src = ./kinect_upload_fw;
          makeFlags = ["PREFIX=$(out)"];
        };

      udev-rules = with pkgs;
        stdenv.mkDerivation rec {
          name = "kinect-udev-rules";

          src = ./contrib;
          RULES = "55-kinect_audio.rules";
          RULES_IN = "${RULES}.in";

          patchPhase = ''
            substitute ${RULES_IN} ${RULES} \
              --subst-var-by LOADER_PATH ${upload}/bin/kinect_upload_fw \
              --subst-var-by FIRMWARE_PATH ${firmware}
          '';

          installPhase = ''
            install -D ${RULES} $out/lib/udev/rules.d/${RULES}
          '';
        };

      default = udev-rules;
    };

    nixosModules.default = {system, ...}: {
      services.udev.packages = [self.packages."${system}".default];
    };
  };
}
