# Wine WoW64 on Windows

This port runs Wine's x86 PE loader and WoW64 conversion code inside a native
x64 Windows process. Linux and MinGW cross-compile the Windows runtime.
GUI, ARM64 and additional NT services are outside the current scope.

Build with Nix on x86_64 Linux:

```sh
nix build
nix run -- ./wine-wow64-windows
```

The first command produces the store output. The second copies it to a writable
directory. On Windows, change into its `runtime` directory and run:

```powershell
.\wine-host.exe --wine-components C:\path\to\application.exe
```

Nix pins its toolchain through `flake.lock`. Wine is the hash-pinned 11.0 release
archive, corresponding to tag `wine-11.0`, commit
`db11d0fe6a169c457e23d007e20404643d067aa8`. The build applies source patches,
regenerates Wine headers and PE DLLs, then compiles the Windows host adapters
and original Wine WoW64 translation units. No locally installed MinGW, copied
Wine installation, prebuilt DLL or absolute developer store path is an input.
The Linux Wine build supplies build tools and PE components. Its Unix runtime
is not shipped as the Windows execution backend.

`patches/` records changes to upstream Wine. `nix/` and the cross-build scripts
record compilation differences. The adapter sources under `prototype/` are
currently required port code, including Windows storage, thread and exception
integration. Extracted upstream routines and generated headers are build
outputs rather than separately maintained implementations.

Tests are a separate Nix output:

```sh
nix build '.#default^tests' --out-link result-tests
```

Their example native x64 bridge DLL and test EXEs are excluded from the runtime
output. `WINE_NATIVE_BRIDGE` can name an explicit native bridge DLL. Test runners explicitly select their `native64.dll` fixture through that
variable. Standard applications can run without a native bridge DLL. Test runners and diagnostic
helpers are not required at runtime. The optional `nix build .#corpus` downloads hash-pinned unmodified
applications. Copy that output to `prototype/unmodified` beside the test
runners. Windows VM execution remains a separate validation step. Mutable
vendor URLs deliberately fail if their content no longer matches the pinned
hash.

This is still a port in development. The prior local build passed the headless
application, sockets, APC, IOCP, context and stack controls described in
[the compatibility report](prototype/README.txt). The Nix rebuild is undergoing
validation; those previous results do not establish its runtime equivalence.
Fiber switching passes, but fiber overflow recovery currently fails. Dynamic
code prohibition and CET compatibility also remain open. Universal unmodified
application compatibility is not established.
