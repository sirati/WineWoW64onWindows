{ runCommand, pkgsCross }:
let cc = pkgsCross.mingwW64.buildPackages.gcc;
in runCommand "wine-shadow-stack-diagnostic" {
  nativeBuildInputs = [ cc ];
  hardeningDisable = [ "all" ];
} ''
  mkdir -p $out
  compiler=${cc}/bin/x86_64-w64-mingw32-gcc
  for helper in diagnose-shadow-stack diagnose-shadow-stack-lowva policy-launch64; do
    flags=()
    case "$helper" in
      diagnose-shadow-stack) source=${../prototype/diagnose-shadow-stack.c} ;;
      diagnose-shadow-stack-lowva)
        source=${../prototype/diagnose-shadow-stack.c}
        flags+=( -Wl,--image-base,0x18000000,--disable-high-entropy-va ) ;;
      policy-launch64) source=${../prototype/policy_launch64.c} ;;
    esac
    "$compiler" -O2 -nostdlib "$source" "''${flags[@]}" \
      "$($compiler -print-file-name=crt2.o)" "$($compiler -print-file-name=crtbegin.o)" \
      -lmingw32 -lgcc -lgcc_eh -lmingwex -lmsvcrt -lkernel32 \
      "$($compiler -print-file-name=crtend.o)" -o "$out/$helper.exe"
  done
''
