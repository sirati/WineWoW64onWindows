# Wine WoW64 on Windows

This port runs Wine's x86 PE loader and WoW64 conversion code inside a native
x64 Windows process. Linux and MinGW cross-compile the Windows runtime.
GUI, ARM64 and additional NT services are outside the current scope.

Build with Nix on x86_64 Linux:

```sh
nix build
nix run . -- ./wine-wow64-windows
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
output. `WINE_NATIVE_BRIDGE` can name an explicit native bridge DLL. Test runners
select their `native64.dll` fixture through that variable. Standard applications
run without a native bridge DLL. The optional `nix build .#corpus` downloads
hash-pinned unmodified applications. Copy that output to `prototype/unmodified`
beside the test runners. Mutable vendor URLs fail if their content no longer
matches the pinned hash.

Windows VM execution is a separate validation step. Luna 6 runs those tests
from a frozen, separate Git worktree. The test bundle records the commit and
every deployed file's SHA256. Test runners and diagnostics are not runtime
dependencies. The standalone image-mapping and context-continuation helpers
are available through `nix build .#image-mapping-diagnostic` and
`nix build .#continuation-diagnostic`. `nix develop` supplies Python for source
and evidence helper scripts.

This is still a port in development. Nix candidate `d34a200` passed the required
headless application, socket, APC, IOCP, callback, thread, context, XSTATE,
stack and fiber controls. Original Wine version and debug routines also pass.
See [the compatibility report](prototype/README.txt) and
[remaining work](prototype/REMAINING-WORK.txt) for the tested scope and limits.
The image-backed bootstrap also runs a guest with dynamic-code prohibition
enabled. See [the frozen candidate report](docs/validation-d34a200.txt) for
hashes and acceptance results. CET remains open. Universal unmodified
application compatibility is not established.
