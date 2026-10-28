# Upstream test sources and the STANDALONE Wine test runner are unmodified.
# This output is test input only and is excluded from the shipped runtime.
{ runCommand, fetchurl, pkgsCross, python3 }:
let
  source = import ./wine-source.nix { inherit fetchurl; };
  cc = pkgsCross.mingw32.buildPackages.gcc;
in runCommand "wine-11.0-upstream-x86-tests" {
  nativeBuildInputs = [ cc python3 ];
  hardeningDisable = [ "all" ];
} ''
  mkdir -p source $out
  tar -xf ${source} -C source --strip-components=1
  compiler=${cc}/bin/i686-w64-mingw32-gcc
  "$compiler" -O2 -D__WINE_PE_BUILD -D__WINESRC__ -I source/include -include _mingw.h \
    -c source/dlls/winecrt0/debug.c -o debug.o
  # The standalone runner uses console declarations that each test source
  # need not include. Load NT status definitions before suppressing the
  # duplicate SDK definitions, matching Wine's usual header order.
  cat > test-preinclude.h <<'EOF'
  #include <_mingw.h>
  #include "ntstatus.h"
  #define WIN32_NO_STATUS
  #include "windef.h"
  #include "winbase.h"
  #include "wincon.h"
  EOF
  for test in ntdll/rtlbitmap ntdll/large_int ntdll/error ntdll/rtlstr \
      ntdll/string ntdll/time ntdll/env kernel32/fiber kernel32/heap \
      kernel32/thread kernel32/sync msvcrt/cpp; do
    module="''${test%/*}"
    name="''${test#*/}"
    "$compiler" -O2 -g -DSTANDALONE -D__WINE_PE_BUILD \
      -D__WINESRC__ -I source/include -iquote source/include/msvcrt \
      -include ./test-preinclude.h \
      -nostdlib "source/dlls/$module/tests/$name.c" debug.o \
      "$($compiler -print-file-name=crt2.o)" "$($compiler -print-file-name=crtbegin.o)" \
      -Wl,--dynamicbase,--nxcompat,--no-insert-timestamp -lmingw32 -lgcc -lgcc_eh -lmingwex \
      -lmsvcrt -lkernel32 -luser32 -ladvapi32 -lntdll \
      "$($compiler -print-file-name=crtend.o)" -o "$out/$module-$name.exe"
  done
  python3 - "$out" ${source} <<'PY'
  import hashlib, json, pathlib, sys
  out = pathlib.Path(sys.argv[1])
  rows = []
  for exe in sorted(out.glob('*.exe')):
      module, name = exe.stem.split('-', 1)
      path = pathlib.Path('source/dlls') / module / 'tests' / (name + '.c')
      rows.append({'program': exe.name, 'module': module, 'test': name,
                   'source': str(path.relative_to('source')),
                   'sourceSHA256': hashlib.sha256(path.read_bytes()).hexdigest(),
                   'sha256': hashlib.sha256(exe.read_bytes()).hexdigest()})
  (out / 'manifest.json').write_text(json.dumps({
      'wineRevision': 'db11d0fe6a169c457e23d007e20404643d067aa8',
      'wineArchive': sys.argv[2], 'architecture': 'i386',
      'runner': 'unmodified include/wine/test.h STANDALONE',
      'helpers': [{'source': name,
                   'sourceSHA256': hashlib.sha256((pathlib.Path('source') / name).read_bytes()).hexdigest()}
                  for name in ('include/wine/test.h', 'dlls/winecrt0/debug.c')],
      'tests': rows}, indent=2) + '\n')
  PY
''
