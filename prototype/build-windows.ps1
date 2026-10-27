param([string]$Toolchain='C:/tools/llvm-mingw-20261020-ucrt-x86_64/bin')
$ErrorActionPreference='Stop'
Set-Location $PSScriptRoot
$tools=$Toolchain
$cc64="$tools/x86_64-w64-mingw32-gcc.exe"
$cc32="$tools/i686-w64-mingw32-gcc.exe"
$inc=@('-I../wine-11.0-source/include','-Iinclude/wine/windows','-Iinclude','-I.')
$flags=@('-O2','-g','-std=gnu17','-D__WINESRC__','-D__WINE_PE_BUILD','-D__USE_MINGW_ANSI_STDIO=0','-include','compat.h','-ffunction-sections','-fdata-sections')
New-Item -ItemType Directory -Force out/guest | Out-Null
Start-Transcript -Path out/build-windows.log -Force
function Compile($cc,$argv) { & $cc @argv; if($LASTEXITCODE) { throw "Compiler exited $LASTEXITCODE" } }
$compilerVersion=& $cc64 --version
$compilerVersion|Set-Content out/compiler-version.txt
$compilerVersion|Write-Output
Compile $cc64 @('-O2','-shared','-static','native64.c','-o','out/native64.dll','-luser32')
Compile $cc32 ($flags+$inc+@('-shared','-nostdlib','guest_ntdll.c','guest_ntdll.def','-Wl,--entry,DllMain@12,--image-base,0x10000000,--kill-at,--out-implib,out/libguest_ntdll.a','-o','out/guest/ntdll.dll'))
Compile $cc32 ($flags+$inc+@('-shared','-nostdlib','guest_kernel32.c','guest_kernel32.def','out/libguest_ntdll.a','-Wl,--entry,DllMain@12,--image-base,0x11000000,--kill-at,--out-implib,out/libguest_kernel32.a','-o','out/guest/kernel32.dll'))
Compile $cc32 @('-O2','-D_WIN32_WINNT=0x0601','-ffreestanding','-fno-stack-protector','-nostdlib','hello32.c','out/libguest_kernel32.a','-Wl,--entry,_app_entry@0,--subsystem,console,--image-base,0x400000','-o','out/guest/hello32.exe')
foreach($test in @('stress32','teb32','tls32','threads32','files32','dynamic32')) {
    Compile $cc32 @('-O2','-D_WIN32_WINNT=0x0601','-ffreestanding','-fno-stack-protector','-nostdlib',"$test.c",'out/libguest_kernel32.a','-lkernel32','-Wl,--entry,_app_entry@0,--subsystem,console,--image-base,0x400000','-o',"out/guest/$test.exe")
}
Compile $cc32 @('-O2','-D_WIN32_WINNT=0x0601','-nostdlib','crt32.c','-lucrtbase','-lkernel32','-Wl,--entry,_app_entry@0,--subsystem,console,--image-base,0x400000','-o','out/guest/crt32.exe')
foreach($unit in @('file','process','registry','security','sync','syscall','system','virtual')) { Compile $cc64 ($flags+$inc+@('-c',"../wine-11.0-source/dlls/wow64/$unit.c",'-o',"out/wine-$unit.o")) }
Compile $cc64 ($flags+$inc+@('-fno-asynchronous-unwind-tables','-fno-unwind-tables','-c','wine_close.c','-o','out/wine-close.o'))
Compile $cc64 ($flags+$inc+@('-static','host.c','backend.c','wine_cpu_adapter.c','out/wine-file.o','out/wine-process.o','out/wine-virtual.o','out/wine-sync.o','out/wine-close.o','-Wl,--gc-sections,--image-base,0x60000000,--disable-dynamicbase','-o','out/wine-host.exe','-lntdll'))
Stop-Transcript
