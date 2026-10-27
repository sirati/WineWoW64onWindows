{ runCommand, pkgsCross }:
let
  cc64 = pkgsCross.mingwW64.buildPackages.gcc;
  cc32 = pkgsCross.mingw32.buildPackages.gcc;
in runCommand "wine-continuation-diagnostic" {
  nativeBuildInputs = [ cc64 cc32 ];
  hardeningDisable = [ "all" ];
} ''
  mkdir -p $out
  compiler=${cc64}/bin/x86_64-w64-mingw32-gcc
  for helper in diagnose-continuation diagnose-continuation-lowbase diagnose-continuation-lowva policy_launch64; do
    flags=()
    case "$helper" in
      diagnose-continuation) source=${../prototype/diagnose-continuation.c} ;;
      diagnose-continuation-lowbase)
        source=${../prototype/diagnose-continuation.c}
        flags+=( -Wl,--image-base,0x18000000,--high-entropy-va ) ;;
      diagnose-continuation-lowva)
        source=${../prototype/diagnose-continuation.c}
        flags+=( -Wl,--image-base,0x18000000,--disable-high-entropy-va ) ;;
      policy_launch64) source=${../prototype/policy_launch64.c} ;;
    esac
    "$compiler" -O2 -nostdlib "$source" "''${flags[@]}" \
      "$($compiler -print-file-name=crt2.o)" "$($compiler -print-file-name=crtbegin.o)" \
      -lmingw32 -lgcc -lgcc_eh -lmingwex -lmsvcrt -lkernel32 \
      "$($compiler -print-file-name=crtend.o)" -o "$out/$helper.exe"
  done
  ${cc32}/bin/i686-w64-mingw32-gcc -shared -nostdlib \
    ${../prototype/diagnose-continuation32.S} \
    -Wl,--entry,_diagnostic_entry,--image-base,0x22000000,--disable-dynamicbase,--nxcompat \
    -o $out/continuation32.dll
''
