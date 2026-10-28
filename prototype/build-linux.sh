#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

# This is the authoritative build.  It is a Linux-hosted cross build that
# emits Windows PE files.  The Windows VM is only used to execute the output.
cc64=${PROBE_CC64:-x86_64-w64-mingw32-gcc}
cc32=${PROBE_CC32:-i686-w64-mingw32-gcc}
dlltool32=${PROBE_DLLTOOL32:-i686-w64-mingw32-dlltool}
nm32=${PROBE_NM32:-i686-w64-mingw32-nm}
# Clang/lld only generate Microsoft-ABI exception test EXEs. The Wine port
# and the host remain MinGW GCC cross builds.
clang=${PROBE_CLANG:-clang}
lld=${PROBE_LLD_LINK:-lld-link}
inc=(-Iwine-11.0-source/include -Iprototype/include/wine/windows -Iprototype/include -Iprototype)
flags=(-O2 -g -std=gnu17 -D__WINESRC__ -D__WINE_PE_BUILD -DPROBE_NATIVE_WINDOWS -D__USE_MINGW_ANSI_STDIO=0 -include prototype/compat.h -ffunction-sections -fdata-sections)
mitigation_flags=(-Wl,--dynamicbase,--high-entropy-va,--nxcompat)
mkdir -p prototype/out/guest
mkdir -p prototype/out/stock-wine
cp -R prototype/stock-wine/i386-windows/. prototype/out/stock-wine/
if [[ ${PROBE_LOADER_SOURCE_PATCHED:-0} != 1 ]]; then
  python3 prototype/patch-wine-loader.py prototype/out/stock-wine/ntdll.dll
fi
python3 prototype/extract-exceptions.py
python3 prototype/select-wine-thunks.py
python3 prototype/extract-native-sockets.py
python3 prototype/extract-wine-unixlib.py
python3 prototype/extract-wine-process-params.py
source prototype/build-wine-sockets.sh
perl -0777 -ne '/(^static inline void set_async_iosb\([^\n]*\)\n\{.*?^\})/ms or die "Wine async IOSB helper missing";print "$1\n";' \
  wine-11.0-source/dlls/ntdll/unix/unix_private.h > prototype/out/wine-async-iosb.inc
install -m 644 prototype/stock-wine/*.nls prototype/out/stock-wine/
# This pinned Wine PE retains private COFF symbols. The bootstrap calls the
# original loader registration routine; it does not duplicate its indexes.
"$nm32" --defined-only prototype/stock-wine/i386-windows/ntdll.dll | perl -ne '
 BEGIN { @wanted=qw(alloc_module fixup_imports loader_init is_prefix_bootstrap init_user_process_params tls_dirs tls_module_count hash_table ldr loader_section peb_lock default_load_path node_ntdll node_kernel32 imports_fixup_done current_version); }
 if(/^([0-9a-f]+) \w _(.+)$/) { $sym{$2}=hex($1)-0x7bc00000; }
 END { for $n (@wanted) { exists $sym{$n} or die "Wine private symbol missing: $n";printf "#define WINE_NTDLL_%s 0x%xu\n",$n,$sym{$n}; } }' \
 > prototype/out/wine-ntdll-symbols.h
perl -0777 -ne '/(^void init_image_mapping\([^\n]*\)\n\{.*?^\})/ms or die "Wine image hook missing";print "$1\n";' \
  wine-11.0-source/dlls/wow64/syscall.c > prototype/out/wine-image-mapping.inc
perl -0777 -ne 'for $fn ("wow64_NtClose","wow64_NtGetCurrentProcessorNumber","wow64_NtQueryDefaultLocale","wow64_NtQueryDefaultUILanguage","wow64_NtWow64IsProcessorFeaturePresent") { /(^NTSTATUS WINAPI \Q$fn\E\([^\n]*\)\n\{.*?^\})/ms or die "Wine thunk missing: $fn"; print "$1\n"; }' \
  wine-11.0-source/dlls/wow64/syscall.c > prototype/out/wine-base-thunks.inc


# The Nix GCC target uses the mcf thread model, but this focused bridge does
# not link libmcfgthread.  These objects do not use the compiler TLS runtime.
winlibs=(-lmingw32 -lgcc -lgcc_eh -lmingwex -lmsvcrt -lkernel32 -ladvapi32 -lshell32 -luser32)
crt2=$($cc64 -print-file-name=crt2.o)
crtbegin=$($cc64 -print-file-name=crtbegin.o)
crtend=$($cc64 -print-file-name=crtend.o)

"$cc64" -O2 -g -nostdlib prototype/debug-guest.c "$crt2" "$crtbegin" -ldbghelp "${winlibs[@]}" "$crtend" -o prototype/out/debug-guest.exe
"$cc64" -O2 -g -nostdlib prototype/async_peer64.c "$crt2" "$crtbegin" "${winlibs[@]}" "$crtend" -o prototype/out/async-peer64.exe
"$cc64" -O2 -g -nostdlib prototype/policy_launch64.c "$crt2" "$crtbegin" "${winlibs[@]}" "$crtend" -o prototype/out/policy-launch64.exe
"$cc64" -O2 -g -nostdlib prototype/unixlib_window_native.c "$crt2" "$crtbegin" "${winlibs[@]}" "$crtend" -o prototype/out/unixlib-window-native.exe

"$cc64" --version > prototype/out/compiler-version-linux.txt
"$cc64" "${flags[@]}" -shared -nostdlib prototype/native64.c -o prototype/out/native64.dll \
  -Wl,--kill-at -Wl,--subsystem,windows "${mitigation_flags[@]}" "${winlibs[@]}"
"$cc32" "${flags[@]}" "${inc[@]}" -shared -nostdlib prototype/guest_ntdll.c prototype/guest_ntdll.def \
  -Wl,--entry,DllMain@12,--image-base,0x10000000,--add-stdcall-alias,--out-implib,prototype/out/libguest_ntdll.a \
  -o prototype/out/guest/ntdll.dll
"$dlltool32" --input-def prototype/wine-memory.def --output-lib prototype/out/libwine-memory.a
"$cc32" "${flags[@]}" "${inc[@]}" -shared -nostdlib prototype/guest_bootstrap.c prototype/out/libwine-memory.a -lntdll -lkernel32 \
  -Wl,--entry,DllMain@12,--image-base,0x12000000,--add-stdcall-alias -o prototype/out/guest/bootstrap32.dll
"$cc32" "${flags[@]}" "${inc[@]}" -shared -nostdlib prototype/legacy_library32.c \
  -Wl,--entry,DllMain@12,--image-base,0x13000000,--add-stdcall-alias,--out-implib,prototype/out/liblegacy-library.a \
  -o prototype/out/guest/legacy_library.dll
"$cc32" "${flags[@]}" "${inc[@]}" -shared -nostdlib prototype/guest_kernel32.c prototype/guest_kernel32.def \
  prototype/out/libguest_ntdll.a -Wl,--entry,DllMain@12,--image-base,0x11000000,--add-stdcall-alias,--out-implib,prototype/out/libguest_kernel32.a \
  -o prototype/out/guest/kernel32.dll

# Static image-backed gates contain both x86 entries and x64 trampolines.
"$cc32" -shared -nostdlib prototype/transition32.S prototype/transition32.def \
  -Wl,--entry,0,--image-base,0x20000000,--dynamicbase,--nxcompat \
  -o prototype/out/guest/transition32.dll
python3 prototype/check-transition-image.py prototype/out/guest/transition32.dll

guest_flags=(-O2 -D_WIN32_WINNT=0x0601 -ffreestanding -fno-stack-protector -nostdlib)
"$cc32" "${guest_flags[@]}" prototype/hello32.c -lkernel32 prototype/out/libguest_kernel32.a \
  -Wl,--entry,_app_entry@0,--subsystem,console,--image-base,0x400000 -o prototype/out/guest/hello32.exe
for test in stress32 teb32 tls32 tls_slots32 threads32 files32 dynamic32 file_semantics32 heap32 memory_status32 return32 concurrent32 fs_recovery32 async_event32 pipe_control32 apc32 io_notify32 io_thread_exit32 thread_context32 thread_exit_context32 thread_delayed_context32 thread_create32 thread_priority32 selector_identity32 activation_context32 worker_process_exit32 threadpool_wait32 condition_wake32 thread_affinity32 thunk_fault32 xstate32 xstate_components32 fiber32 fiber_thread_exit32 wine_debug32 tick_count32 stack_growth32 mitigation32 process_parameters32 named_objects32; do
  test_flags=();if [[ "$test" == xstate32 ]];then test_flags+=( -msse2 );fi
  if [[ "$test" == process_parameters32 || "$test" == named_objects32 ]];then test_flags+=( "${flags[@]}" "${inc[@]}" );fi
  "$cc32" "${guest_flags[@]}" "${test_flags[@]}" prototype/$test.c -lkernel32 prototype/out/libguest_kernel32.a \
    -Wl,--entry,_app_entry@0,--subsystem,console,--image-base,0x400000 -o prototype/out/guest/$test.exe
done
for mode in normal laa; do
  test_flags=();test=process_view32
  if [[ "$mode" == laa ]];then test_flags+=( -DTEST_LARGE_ADDRESS_AWARE -Wl,--large-address-aware );test=process_view_laa32;fi
  "$cc32" "${guest_flags[@]}" "${test_flags[@]}" prototype/process_view32.c -lkernel32 \
    -Wl,--entry,_app_entry@0,--subsystem,console,--image-base,0x400000 -o prototype/out/guest/$test.exe
done
for test in socket_dns32 socket32 socket_extensions32; do
  "$cc32" "${guest_flags[@]}" prototype/$test.c -lws2_32 -lkernel32 \
    -Wl,--entry,_app_entry@0,--subsystem,console,--image-base,0x400000 -o prototype/out/guest/$test.exe
done
"$cc32" "${guest_flags[@]}" -shared prototype/socket_library32.c -lws2_32 \
  -Wl,--entry,_DllMain@12,--kill-at -o prototype/out/guest/socket_library32.dll
"$cc32" "${guest_flags[@]}" -shared prototype/dynamic_tls_library32.c \
  -Wl,--entry,_DllMain@12,--kill-at -o prototype/out/guest/dynamic_tls_library32.dll
"$cc32" "${guest_flags[@]}" -shared prototype/process_exit_library32.c -lkernel32 \
  -Wl,--entry,_DllMain@12,--kill-at -o prototype/out/guest/process_exit_library32.dll
"$cc32" "${guest_flags[@]}" -shared prototype/process_start_library32.c -lkernel32 \
  -Wl,--entry,_DllMain@12,--kill-at,--out-implib,prototype/out/libprocess-start.a \
  -o prototype/out/guest/process_start_library32.dll
"$cc32" "${guest_flags[@]}" "${flags[@]}" "${inc[@]}" prototype/process_create32.c \
  prototype/out/libprocess-start.a -lkernel32 \
  -Wl,--entry,_app_entry@0,--subsystem,console,--image-base,0x400000 -o prototype/out/guest/process_create32.exe
"$cc32" -O2 -nostdlib prototype/disk_loader32.c \
  "$($cc32 -print-file-name=crt2.o)" "$($cc32 -print-file-name=crtbegin.o)" \
  "${winlibs[@]}" "$($cc32 -print-file-name=crtend.o)" -o prototype/out/guest/disk_loader32.exe
"$cc32" -O2 -D_WIN32_WINNT=0x0601 -nostdlib prototype/crt32.c -lmsvcrt -lkernel32 \
  -Wl,--entry,_app_entry@0,--subsystem,console,--image-base,0x400000 -o prototype/out/guest/crt32.exe
"$cc32" "${guest_flags[@]}" prototype/bridge_app32.c prototype/out/liblegacy-library.a -lmsvcrt -lkernel32 \
  -Wl,--entry,_app_entry@0,--subsystem,console,--image-base,0x400000 -o prototype/out/guest/bridge_app32.exe
for test in callback32 callback_worker32; do
  "$cc32" "${guest_flags[@]}" -msse2 prototype/$test.c prototype/out/liblegacy-library.a -lkernel32 \
    -Wl,--entry,_app_entry@0,--subsystem,console,--image-base,0x400000 -o prototype/out/guest/$test.exe
done

"$cc32" "${guest_flags[@]}" -shared prototype/lifecycle_dll32.c -lkernel32 \
  -Wl,--entry,_DllMain@12,--image-base,0x14000000,--add-stdcall-alias,--out-implib,prototype/out/liblifecycle.a \
  -o prototype/out/guest/lifecycle_dll32.dll
"$cc32" "${guest_flags[@]}" prototype/lifecycle32.c prototype/out/liblifecycle.a -lkernel32 \
  -Wl,--entry,_app_entry@0,--subsystem,console,--image-base,0x400000 -o prototype/out/guest/lifecycle32.exe

"$clang" --version > prototype/out/exception-compiler-version.txt
for test in cpp_unwind32 cpp_nested_return32; do
"$clang" --target=i686-pc-windows-msvc -fms-compatibility-version=16 \
  -fexceptions -fcxx-exceptions -fno-rtti -fno-stack-protector -O1 \
  -c prototype/$test.cpp -o prototype/out/$test.obj
"$lld" /nodefaultlib /safeseh:no /subsystem:console /entry:app_entry \
  /out:prototype/out/guest/$test.exe prototype/out/$test.obj \
  "$($cc32 -print-file-name=libmsvcr100.a)" "$($cc32 -print-file-name=libkernel32.a)"
"$lld" /nodefaultlib /safeseh:no /subsystem:console /entry:app_entry \
  /out:prototype/out/guest/$test-native.exe prototype/out/$test.obj \
  "$($cc32 -print-file-name=libmsvcrt.a)" "$($cc32 -print-file-name=libkernel32.a)"
done
for test in seh_fault32 stack_overflow32 fiber_overflow32; do
"$clang" --target=i686-pc-windows-msvc -fms-compatibility-version=16 \
  -fms-extensions -fasync-exceptions -fno-stack-protector -O1 \
  -c prototype/$test.c -o prototype/out/$test.obj
"$lld" /nodefaultlib /safeseh:no /subsystem:console /entry:app_entry \
  /out:prototype/out/guest/$test.exe prototype/out/$test.obj \
  "$($cc32 -print-file-name=libmsvcrt.a)" "$($cc32 -print-file-name=libkernel32.a)"
done

"$cc64" "${flags[@]}" "${inc[@]}" \
  -c prototype/wine_close.c -o prototype/out/wine-close.o

for unit in file process registry security sync syscall system virtual; do
  extra=()
  if [[ "$unit" == syscall ]]; then
    extra=(-DDllMain=wine_wow64_DllMain -DWow64SystemServiceEx=wine_wow64_Wow64SystemServiceEx \
      -Dinit_image_mapping=wine_wow64_init_image_mapping \
      -DWow64AllocateTemp=wine_wow64_Wow64AllocateTemp -DWow64ApcRoutine=wine_wow64_Wow64ApcRoutine \
      -Dnative_machine=wine_wow64_native_machine -Dcurrent_machine=wine_wow64_current_machine \
      -Dargs_alignment=wine_wow64_args_alignment -Dhighest_user_address=wine_wow64_highest_user_address \
      -Ddefault_zero_bits=wine_wow64_default_zero_bits \
      -DpBTCpuFlushInstructionCache2=wine_wow64_pBTCpuFlushInstructionCache2 \
      -DpBTCpuNotifyMapViewOfSection=wine_wow64_pBTCpuNotifyMapViewOfSection \
      -DpBTCpuNotifyMemoryAlloc=wine_wow64_pBTCpuNotifyMemoryAlloc \
      -DpBTCpuNotifyMemoryFree=wine_wow64_pBTCpuNotifyMemoryFree \
      -DpBTCpuNotifyMemoryProtect=wine_wow64_pBTCpuNotifyMemoryProtect \
      -DpBTCpuNotifyReadFile=wine_wow64_pBTCpuNotifyReadFile \
      -DpBTCpuNotifyUnmapViewOfSection=wine_wow64_pBTCpuNotifyUnmapViewOfSection)
  fi
  if [[ "$unit" == sync ]];then extra+=( -DNtOpenDirectoryObject=probe_NtOpenDirectoryObject );fi
  if [[ "$unit" == process ]]; then
    extra+=( -DNtAlertMultipleThreadByThreadId=probe_NtAlertMultipleThreadByThreadId )
    extra+=( -DRtlWow64GetThreadSelectorEntry=probe_RtlWow64GetThreadSelectorEntry )
    extra+=( -DNtQueryInformationProcess=probe_NtQueryInformationProcess )
    extra+=( -DNtCreateUserProcess=probe_NtCreateUserProcess )
    extra+=( -DNtSetInformationProcess=probe_NtSetInformationProcess -DNtQueueApcThread=probe_NtQueueApcThread -DNtCreateThreadEx=probe_NtCreateThreadEx -DNtTerminateThread=probe_NtTerminateThread -DNtQueryInformationThread=probe_NtQueryInformationThread -DNtSetInformationThread=probe_NtSetInformationThread -DNtGetNextThread=probe_NtGetNextThread )
  fi
  if [[ "$unit" == system ]]; then extra+=( -DNtQuerySystemInformation=probe_NtQuerySystemInformation );fi
  if [[ "$unit" == sync ]]; then extra+=( -DNtWaitForSingleObject=probe_NtWaitForSingleObject -DNtWaitForMultipleObjects=probe_NtWaitForMultipleObjects -DNtDuplicateObject=probe_NtDuplicateObject );fi
  if [[ "$unit" == virtual ]]; then extra+=( -DNtFreeVirtualMemory=probe_NtFreeVirtualMemory -DNtGetNlsSectionPtr=probe_NtGetNlsSectionPtr -DNtInitializeNlsFiles=probe_NtInitializeNlsFiles -DNtQueryVirtualMemory=probe_NtQueryVirtualMemory );fi
  if [[ "$unit" == file ]]; then extra+=( -DNtDeviceIoControlFile=probe_NtDeviceIoControlFile -DNtLockFile=probe_NtLockFile -DNtOpenFile=probe_NtOpenFile -DNtQueryInformationFile=probe_NtQueryInformationFile -DNtSetInformationFile=probe_NtSetInformationFile -DNtReadFile=probe_NtReadFile -DNtWriteFile=probe_NtWriteFile -DNtCancelIoFileEx=probe_NtCancelIoFileEx -DNtFsControlFile=probe_NtFsControlFile -DNtCreateNamedPipeFile=probe_NtCreateNamedPipeFile );fi
  unit_source=prototype/out/wine-$unit.c
  if [[ "$unit" == syscall ]];then unit_source=wine-11.0-source/dlls/wow64/syscall.c;fi
  "$cc64" "${flags[@]}" "${extra[@]}" "${inc[@]}" -Iwine-11.0-source/dlls/wow64 \
    -c "$unit_source" -o prototype/out/wine-$unit.o
done
for host_variant in normal no-unixlib;do
host_flags=();host_name=wine-host
if [[ "$host_variant" == no-unixlib ]];then host_flags+=( -DPROBE_TEST_NO_NATIVE_UNIXLIB );host_name=wine-host-no-unixlib;fi
"$cc64" "${flags[@]}" "${host_flags[@]}" "${inc[@]}" prototype/host.c prototype/backend.c prototype/wine_cpu_adapter.c prototype/wine_exceptions.c prototype/wine_extended_context.c prototype/xstate.c prototype/native_io.c prototype/native_sockets.c prototype/native_socket_io.c prototype/native_threads.c prototype/native_loader.c prototype/native_debug.c prototype/native_process_params.c prototype/native_process.c \
  prototype/out/wine-file.o prototype/out/wine-process.o prototype/out/wine-sync.o prototype/out/wine-virtual.o \
  prototype/out/wine-registry.o \
  prototype/out/wine-system.o prototype/out/wine-security.o \
  prototype/out/wine-close.o \
  -Wl,--gc-sections,--image-base,0x60000000 "${mitigation_flags[@]}" \
  -Wl,--subsystem,console -Wl,--entry,probe_mainCRTStartup -nostdlib -o "prototype/out/$host_name.exe" \
  "$crt2" "$crtbegin" "${winlibs[@]}" -lntdll -lws2_32 "$crtend"
python3 prototype/check-native-unwind.py "prototype/out/$host_name.exe"
done

echo "cross-build complete: prototype/out/wine-host.exe"
