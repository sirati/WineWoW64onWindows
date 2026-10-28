{ lib, stdenv, fetchurl, wineWow64Packages, pkgsCross, python3, perl,
  clang, lld, project }:
let
  wineSource = import ./wine-source.nix { inherit fetchurl; };
  wine = (wineWow64Packages.minimal.override {
    mingwSupport = true;
    embedInstallers = false;
  }).overrideAttrs (old: {
    version = "11.0";
    src = wineSource;
    patches = [
      ../patches/0001-loader-request-execute-access.patch
      ../patches/0003-native-host-process-return.patch
      ../patches/0004-shared-tick-multiplier.patch
      ../patches/0006-refresh-process-memory-status.patch
    ];
    dontStrip = true; # Wine loader private symbols are resolved during the cross build.
  });
  # Rebuild only the shipped x86 CRTs when their shared exception code changes.
  # The remaining Wine PEs and native build tools use the existing pinned build.
  wineCxx = wine.overrideAttrs (old: {
    pname = "wine-x86-cxx-runtime";
    patches = old.patches ++ [ ../patches/0008-restore-catch-stack-on-rethrow.patch ];
    configureFlags = lib.filter (flag: !(lib.hasPrefix "--enable-archs=" flag)) old.configureFlags
      ++ [ "--enable-archs=i386" "--disable-tests" ];
    buildPhase = ''
      runHook preBuild
      make -j$NIX_BUILD_CORES dlls/msvcrt/i386-windows/msvcrt.dll \
        dlls/msvcr100/i386-windows/msvcr100.dll dlls/ucrtbase/i386-windows/ucrtbase.dll
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      mkdir -p $out/lib/wine/i386-windows
      cp dlls/msvcrt/i386-windows/msvcrt.dll dlls/msvcr100/i386-windows/msvcr100.dll \
        dlls/ucrtbase/i386-windows/ucrtbase.dll $out/lib/wine/i386-windows/
      runHook postInstall
    '';
  });
  cc32 = pkgsCross.mingw32.buildPackages.gcc;
  cc64 = pkgsCross.mingwW64.buildPackages.gcc;
  bin32 = pkgsCross.mingw32.buildPackages.binutils;
in stdenv.mkDerivation {
  pname = "wine-wow64-windows";
  version = "11.0";
  src = lib.cleanSourceWith {
    src = project;
    filter = path: type:
      let relative = lib.removePrefix (toString project + "/") (toString path);
      in !(lib.any (p: relative == p || lib.hasPrefix (p + "/") relative)
        [ ".git" "evidence" "wine-11.0-source" "prototype/out"
          "prototype/stock-wine" "prototype/include" "prototype/unmodified" ])
        && !(lib.hasSuffix ".tar.gz" relative || lib.hasSuffix ".sha256" relative);
  };
  nativeBuildInputs = [ python3 perl cc32 cc64 bin32 clang lld ];
  outputs = [ "out" "tests" ];
  # PE mitigation flags are supplied explicitly by the cross-build script.
  hardeningDisable = [ "all" ];
  dontConfigure = true;
  dontStrip = true;
  PROBE_CC32 = "${cc32}/bin/i686-w64-mingw32-gcc";
  PROBE_CC64 = "${cc64}/bin/x86_64-w64-mingw32-gcc";
  PROBE_DLLTOOL32 = "${bin32}/bin/i686-w64-mingw32-dlltool";
  PROBE_NM32 = "${bin32}/bin/i686-w64-mingw32-nm";
  PROBE_CLANG = "${clang}/bin/clang";
  PROBE_LLD_LINK = "${lld}/bin/lld-link";
  PROBE_WINEBUILD = "${wine}/bin/winebuild";
  PROBE_LOADER_SOURCE_PATCHED = "1";
  buildPhase = ''
    runHook preBuild
    mkdir wine-11.0-source
    tar -xf ${wineSource} -C wine-11.0-source --strip-components=1
    patch -d wine-11.0-source -p1 < ${../patches/0002-port-extracted-sources.patch}
    patch -d wine-11.0-source -p1 < ${../patches/0005-hypervisor-shared-page.patch}
    patch -d wine-11.0-source -p1 < ${../patches/0007-thread-group-affinity-conversion.patch}
    mkdir -p prototype/include prototype/stock-wine/i386-windows
    cp -R ${wine}/include/wine prototype/include/
    mkdir -p evidence
    python3 prototype/collect-wine-dlls.py ${wine}/lib/wine/i386-windows
    cp ${wineCxx}/lib/wine/i386-windows/*.dll prototype/stock-wine/i386-windows/
    cp ${wine}/share/wine/nls/{l_intl,locale,normnfc,sortdefault}.nls prototype/stock-wine/
    bash prototype/build-linux.sh
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    mkdir -p $out
    mkdir -p $out/runtime/guest $out/docs $tests/prototype
    cp prototype/out/wine-host.exe $out/runtime/
    cp -R prototype/out/stock-wine $out/runtime/
    cp prototype/out/guest/{ntdll,kernel32,bridge_kernel32,bootstrap32,transition32}.dll $out/runtime/guest/
    cp -R prototype/out $tests/prototype/
    find $tests/prototype/out -maxdepth 1 -type f ! -name "*.exe" ! -name "*.dll" ! -name "*.txt" -delete
    cp prototype/*.ps1 $tests/prototype/
    cp prototype/{README,RUNTIME-WORK,REMAINING-WORK}.txt $out/docs/
    cp docs/validation-d34a200.txt $out/docs/
    cp docs/validation-22ec829.txt $out/docs/
    cp docs/validation-d7eaf54.txt $out/docs/
    cp docs/validation-5c02e38.txt $out/docs/
    cp docs/upstream-wine-tests.txt $out/docs/
    cp docs/cet-investigation.txt $out/docs/
    cp docs/loader-relocation-regression.txt $out/docs/
    cat > $out/docs/build-inputs.txt <<EOF
    Wine release: 11.0
    Wine commit: db11d0fe6a169c457e23d007e20404643d067aa8
    Wine archive: ${wineSource}
    Wine PE build: ${wine}
    Wine x86 CRT build: ${wineCxx}
    x86 compiler: ${cc32}
    x64 compiler: ${cc64}
    EOF
    runHook postInstall
  '';
}
