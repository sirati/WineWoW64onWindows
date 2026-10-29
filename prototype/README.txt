Wine 11 WoW64 on native Windows

This prototype keeps application loading inside Wine while the host process is a native x64 Windows process. Linux cross-compilation uses MinGW-w64 GCC 16.2.0. The VM only executes the resulting PE files.

Build:

  nix build
  nix run . -- ./wine-wow64-windows

The build extracts Wine 11 WoW64 conversion and dispatch code, cross-builds Wine's ws2_32 frontend and winecrt0 delay loader, and links native Windows adapters. The Nix expression supplies PROBE_CC64, PROBE_CC32, PROBE_DLLTOOL32 and PROBE_NM32 from its locked toolchain. Clang/lld is used only for the Microsoft ABI exception controls. The historical `build-windows.ps1` does not build the current runtime; use `build-linux.sh`.

Run an unmodified application through Wine:

  .\wine-host.exe --wine-components ..\unmodified\7za920\7za.exe
  .\wine-host.exe --wine-components ..\unmodified\lua542\lua54.exe -e "print(6*7)"

The host maps a small fixed set of bootstrap Wine DLLs with native SEC_IMAGE sections. Wine's PE loader then maps the x86 EXE and its normal dependencies. Windows' native WoW64 EXE loader is not used. Wine's original ntdll, kernelbase, kernel32, msvcrt, loader, exception, CRT and WoW64 conversion code remain in the path.

Current compatibility table

| Area | Result | Evidence and scope |
|---|---|---|
| Unmodified x86 applications | Pass for tested corpus | d34a200: 16/16 checks for 7-Zip create/list/test/extract/overwrite, Lua arithmetic/protected error, SQLite SQL/database reopen, BusyBox help/echo/expr and current 7zr help. Extraction hashes match; native Windows reopens the Wine-created SQLite DB. The example native bridge is unset. |
| x86 DLL to x64 DLL bridge | Pass | Wine loads an x86 replacement DLL from disk; 1,000 checked 64-bit calls above 4 GiB. Final same-thread and native-worker callback fixtures pass five Wine runs each. |
| C++ unwinding and hardware SEH | Pass for tested cases | Wine's original exception matching, dispatcher, CRT and unwind routines pass C++ catches/rethrow/destructors, worker exceptions, access violation, divide-by-zero and `__finally`. The nested returning-helper diagnostic still fails in stock Wine 11 too. |
| Static TLS, FLS and loader lifecycle | Pass | Main/worker attach and detach, dynamic TLS allocation on existing workers, FLS cleanup, normal return and explicit `ExitThread`. Final `thread_context32` passes native control plus five Wine runs. |
| Other-thread contexts | Pass for tested subset | Suspend/get/set/resume of a guest worker, integer/control/debug state and context restoration pass native control plus five Wine runs. Wine original conversion is selected against the guest-thread registry. |
| Thread creation | Pass for tested subset | Suspended creation, nested suspend count, native identity/inheritance, direct `NtCreateThreadEx`, canceled never-started workers and handle cleanup pass native control plus five Wine runs. |
| Native-worker callbacks | Pass | A native x64 worker attaches Wine guest TEB/TLS state once, makes 100 callbacks, preserves guest TLS, and runs Wine DLL/FLS cleanup. Five final Wine runs pass. |
| APC dispatch | Pass | Original Wine APC dispatcher and continuation code deliver nested guest APCs, alertable waits and worker delivery. Native APC control and Five final Wine runs pass. |
| Pending/eventless I/O | Pass | Native request storage plus Wine IOSB conversion handles pending reads/writes, eventless waits, event reuse, duplicate handles, cancellation, issuing-thread exit and closed notifications. Final `io_notify32`: native control plus five Wine runs, with earlier 30-run cancellation stress also accepted. |
| IOCP | Pass | Wine's original create/set/remove completion thunks receive packets after guest IOSB publication. Cancellation, skip-success and batched dequeue pass inside `io_notify32` and socket tests. |
| Named pipes | Pass | Pending connect/transact/peek/disconnect/reconnect and cancellation. Async and pipe controls pass 5 repeats in the final candidate. |
| Socket-backed Wine DLL | Pass for tested scope | Wine's original ws2_32 PE/export table, DNS marshalling, TCP/UDP, nonblocking, select/poll, event select, overlapped/eventless receive, APC/IOCP cancellation, AcceptEx, ConnectEx, TransmitFile and WSARecvMsg IP_PKTINFO. Each final socket fixture passes native control plus five Wine runs. Earlier milestones included 100 socket32 and 30 extension runs. |
| Extended x86 APC variants | Outside current service scope | Basic, initial suspended-thread, nested and I/O completion APC paths are tested. Reserve-object/APCEx2 combinations add services excluded from this phase. |
| XSTATE/AVX | Pass for tested vector components | Wine copy_xstate and extended-context routines handle AVX transitions, APCs, exception-handler edits and suspended-thread get/set/resume. 22ec829 passes native plus five Wine runs of both fixtures, including AVX, opmask, ZMM_Hi256 and Hi16_ZMM across APC delivery and exception-handler context edits. |
| Guard-page stack growth | Pass for tested cases | Per-stack tracking calls Wine grow_thread_stack. d34a200 passes native plus five Wine runs each for growth and repeated overflow/reset, on main and worker stacks. |
| Fiber stacks | Pass for tested cases | Wine stack-allocation requests reserve extra space for native exception dispatch below the logical stack. d34a200 passes native plus five Wine runs each of switching and overflow/reset controls. |
| Mitigations | Partial | Host/native DLL use dynamic-base, high-entropy VA and NX flags. d34a200 passes native plus five Wine section/NX/write-fault controls, ASLR and CFG launches, and hello32 plus ten runtime cases and six unmodified-application commands under dynamic-code prohibition with queried dynamic=1. Bootstrap and gates use image-backed executable pages, with data-only setup. CET remains incompatible with Wine IRETQ. CFG launch compatibility does not establish full guest CFG enforcement. |
| Civ3 GUI, ARM64, extra NT services | Civ3 GUI is active work; ARM64 and extra NT services remain out of scope | Civ3 uses Wine's own win32u, wow64win, wineserver window/message state and nulldrv. It must not route guest user32/gdi32 to the host win32u. |

The latest full regression source is 22ec829c9ae8c6a389919e156dcf5f037835c897. It builds entirely through the pinned Nix expressions. Luna 6 ran it from a separate frozen worktree; Windows verified all 88 input hashes. Host SHA256 is 786E7D019882924BA542AC185FECB6DCE9BD2A3EABD4CC93BBB6197F36A7653E. The cross suite has 29 cases and no unexpected failures; seven required exception cases, 16 unmodified-app checks and 83 focused runtime runs pass. The optional nested-return diagnostic remains. See docs/validation-22ec829.txt for audited counts and the rejected initial stress rows. Earlier dynamic-code acceptance remains in docs/validation-d34a200.txt. The later d7eaf54 change skips empty BSS protection ranges in the main-image relocator and has separate focused validation.

Implementation limits remain. This host requires Windows x64 with usable FSGSBASE instructions, as on the tested Windows 11 VM. The loader bootstrap uses pinned Wine private COFF symbols and is tied to this Wine 11 build. Canonical-address collisions reuse Wine's original relocator; a required executable relocation fails explicitly when dynamic-code policy prohibits it. See docs/loader-relocation-regression.txt. Forced termination while an adapter or native CRT lock is held can deadlock and remains under investigation. The socket adapter covers the standard Winsock controls listed above; provider-specific ioctls need matching adapters. The C++ nested-return diagnostic remains unresolved. The tested headless applications do not establish compatibility with every unmodified application or DLL.

See `RUNTIME-WORK.txt` for the implementation history and exact frozen-candidate notes. Use `bundle-vm-candidate.py` from a clean worktree, upload one ZIP, verify every hash with `verify-candidate.ps1`, and only then run a VM test. Luna 6 is the VM test agent; it does not modify implementation files.

See `REMAINING-WORK.txt` for the unresolved functionality table and estimated difficulty.
