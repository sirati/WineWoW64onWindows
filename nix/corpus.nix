# Optional unmodified x86 validation inputs, kept out of the runtime closure.
{ runCommand, fetchurl, unzip, pkgsCross }:
let
  sevenZip = fetchurl {
    url = "https://www.7-zip.org/a/7za920.zip";
    hash = "sha256-Kjr+GcGA+Dc/oC/wAlTVOU/sA0n1gE4K0vYGeFT/KKw=";
  };
  sevenR = fetchurl {
    url = "https://www.7-zip.org/a/7zr.exe";
    hash = "sha256-JW/sqOJ05dplXiooT6uv2fVUNl6xZIYgidrNToJ20oI=";
  };
  busybox = fetchurl {
    url = "https://frippery.org/files/busybox/busybox.exe";
    hash = "sha256-e/7lMJZTFWZQRObgHbWBJfJ2PIo5wucroaa+tpI+Dh8=";
  };
  lua = fetchurl {
    url = "https://downloads.sourceforge.net/project/luabinaries/5.4.2/Tools%20Executables/lua-5.4.2_Win32_bin.zip";
    hash = "sha256-X1HauPJvx3oSmL4ced6/ld5EV59TCUrlCcW0x8PWrJE=";
  };
  sqlite = fetchurl {
    url = "https://www.sqlite.org/2023/sqlite-tools-win32-x86-3430200.zip";
    hash = "sha256-+unvzPRGnKab5dQY/AiFk824bgehshG9uFRjWRqSOwI=";
  };
  cc = pkgsCross.mingw32.buildPackages.gcc;
in runCommand "wine-wow64-unmodified-test-corpus" {
  nativeBuildInputs = [ unzip cc ];
  preferLocalBuild = true;
} ''
  mkdir -p $out/{7za920,lua542,sqlite343}
  unzip -q ${sevenZip} -d $out/7za920
  unzip -q ${lua} -d $out/lua542
  unzip -q ${sqlite} -d $out/sqlite343
  cp ${sevenR} $out/7zr.exe
  cp ${busybox} $out/busybox.exe
  cp ${../prototype/unmodified/ORIGINS.txt} $out/ORIGINS.txt
  i686-w64-mingw32-gcc ${../prototype/unmodified/ordinary-main.c} -o $out/ordinary-main.exe
''
