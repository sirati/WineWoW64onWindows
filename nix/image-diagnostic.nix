{ runCommand, pkgsCross }:
let cc = pkgsCross.mingwW64.buildPackages.gcc;
in runCommand "wine-image-mapping-diagnostic" {
  nativeBuildInputs = [ cc ];
  hardeningDisable = [ "all" ];
} ''
  mkdir -p $out
  compiler=${cc}/bin/x86_64-w64-mingw32-gcc
  "$compiler" -O2 -nostdlib ${../prototype/diagnose-image-section.c} \
    "$($compiler -print-file-name=crt2.o)" "$($compiler -print-file-name=crtbegin.o)" \
    -lmingw32 -lgcc -lgcc_eh -lmingwex -lmsvcrt -lkernel32 \
    "$($compiler -print-file-name=crtend.o)" -o $out/diagnose-image-section.exe
''
