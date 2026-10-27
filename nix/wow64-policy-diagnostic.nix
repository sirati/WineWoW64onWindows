{ runCommand, pkgsCross }:
let
  cc32 = pkgsCross.mingw32.buildPackages.gcc;
  cc64 = pkgsCross.mingwW64.buildPackages.gcc;
in runCommand "wine-wow64-policy-diagnostic" {
  nativeBuildInputs = [ cc32 cc64 ];
  hardeningDisable = [ "all" ];
} ''
  mkdir -p $out
  for arch in 32 64; do
    if [ "$arch" = 32 ]; then
      compiler=${cc32}/bin/i686-w64-mingw32-gcc
      source=${../prototype/diagnose-wow64-policy.c}
      output=diagnose-wow64-policy
    else
      compiler=${cc64}/bin/x86_64-w64-mingw32-gcc
      source=${../prototype/policy_launch64.c}
      output=policy-launch64
    fi
    "$compiler" -O2 -nostdlib "$source" -Wl,--dynamicbase,--nxcompat \
      "$($compiler -print-file-name=crt2.o)" "$($compiler -print-file-name=crtbegin.o)" \
      -lmingw32 -lgcc -lgcc_eh -lmingwex -lmsvcrt -lkernel32 \
      "$($compiler -print-file-name=crtend.o)" -o "$out/$output.exe"
  done
''
