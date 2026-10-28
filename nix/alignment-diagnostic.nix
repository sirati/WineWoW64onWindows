{ runCommand, pkgsCross }:
let cc = pkgsCross.mingwW64.buildPackages.gcc;
in runCommand "wine-alignment-policy-diagnostic" {
  nativeBuildInputs = [ cc ];
  hardeningDisable = [ "all" ];
} ''
  mkdir -p $out
  ${cc}/bin/x86_64-w64-mingw32-gcc -O2 -shared -nostdlib \
    -I ${../prototype} ${../prototype/diagnose-alignment-policy.c} \
    -Wl,--entry,DllMain,--dynamicbase,--high-entropy-va,--nxcompat,--no-insert-timestamp \
    -lkernel32 -luser32 -o $out/native64-alignment-policy.dll
''
