Wine 11 WoW64 on native Windows

This prototype keeps application loading inside Wine while the host process is a native x64 Windows process. Linux cross-compilation uses MinGW-w64 GCC 16.2.0. The VM only executes the resulting PE files.

Build:

  nix build
  nix run . -- ./wine-wow64-windows

The build extracts Wine 11 WoW64 conversion and dispatch code, cross-builds Wine's ws2_32 frontend and winecrt0 delay loader, and links native Windows adapters. The Nix expression supplies PROBE_CC64, PROBE_CC32, PROBE_DLLTOOL32 and PROBE_NM32 from its locked toolchain. Clang/lld is used only for the Microsoft ABI exception controls. The historical `build-windows.ps1` does not build the current runtime; use `build-linux.sh`.

Run an unmodified application through Wine:

  .\wine-host.exe --wine-components ..\unmodified\7za920\7za.exe
  .\wine-host.exe --wine-components ..\unmodified\lua542\lua54.exe -e "print(6*7)"

The host maps the x86 EXE and DLLs with Wine's PE loader. Windows' native WoW64 EXE loader is not used. Wine's original ntdll, kernelbase, kernel32, msvcrt, loader, exception, CRT and WoW64 conversion code remain in the path.

Current compatibility table

| Area | Result | Evidence and scope |
|---|---|---|
| Unmodified x86 applications | Pass | 7-Zip create/list/test/extract/overwrite, Lua arithmetic/protected error, SQLite SQL/database reopen, BusyBox help/echo/expr, current 7zr help. Final candidate: 16/16 application checks, matching extraction hashes and 50/50 application repeats; zero required failures. |
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
| XSTATE/AVX | Pass for tested AVX cases | Wine `copy_xstate` and extended-context routines now support AVX across transitions, APCs, guest exception-handler edits and remote suspended-thread get/set/resume. Native control and 10 Wine runs pass on `42f387d`; native plus five also pass on `7769bee`. Dedicated `20efe74` controls also pass native plus ten Wine runs for AVX, opmask, ZMM_Hi256 and Hi16_ZMM suspended-thread round trips. This does not extend the AVX exception/APC evidence to every AVX-512 path. |
| Guard-page stack growth | Pass for tested cases | Per-stack reservation/commit/guard tracking calls Wine `grow_thread_stack`. Repeated overflow, SEH unwind, Wine CRT `_resetstkoflw` and further growth pass native and all 10 Wine runs on `7769bee`, on both main and worker stacks. |
| Mitigations | Partial | The host and native DLL use dynamic-base, high-entropy VA and NX-compatible PE flags. Section permissions, NX execution faults and read-only write faults pass native and Wine controls. Forced ASLR and CFG policy launch pass. Dynamic-code prohibition rejects the gate protection change with error 1655. CET fails at Wine IRETQ with fast-fail 0x39, CONTROL_INVALID_RETURN_ADDRESS. CFG policy compatibility does not establish complete guest CFG enforcement. |
| GUI, ARM64, extra NT services | Out of scope | Not part of this prototype. |

The last accepted pre-Nix implementation is commit `7769bee6030d47025f490e67f8f1ffe0458b2e25`, with 139 verified inputs. Deployed host SHA256: `961AD66BC1B910970833D3FCDBDF93BBA7CFC2394A1572936FBAF086C2CCE80A`. Canonical reports record 284 required executions with zero failures, plus matching extraction hashes. One optional stock-Wine nested-return diagnostic still fails. The final evidence and machine-readable summary are under `evidence/final-runtime/`. The packaged test utilities subsequently mark the deliberately FS-clearing recovery test Wine-only by default and add image-base logging to the debugger. The current Nix port includes later changes to bridge loading and fiber-stack allocation. It requires fresh VM acceptance; these historical results do not certify the current generated binaries.

Implementation limits remain. This host requires Windows x64 with usable FSGSBASE instructions, as on the tested Windows 11 VM. The loader bootstrap uses pinned Wine private COFF symbols and is tied to this Wine 11 build. The socket adapter currently targets the standard Winsock controls listed above, not every provider-specific ioctl. The C++ nested-return diagnostic remains unresolved. The tested headless applications do not establish compatibility with every unmodified application or DLL.

See `RUNTIME-WORK.txt` for the implementation history and exact frozen-candidate notes. Use `bundle-vm-candidate.py` from a clean worktree, upload one ZIP, verify every hash with `verify-candidate.ps1`, and only then run a VM test. Luna 6 is the VM test agent; it does not modify implementation files.

See `REMAINING-WORK.txt` for the unresolved functionality table and estimated difficulty.
