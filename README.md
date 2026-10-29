# Wine WoW64 on Windows

This port runs Wine's x86 PE loader and WoW64 conversion code inside a native
x64 Windows process. Linux and MinGW cross-compile the Windows runtime. The
Civ3 target includes Wine's own headless GUI stack (win32u, wow64win,
wineserver window/message state and the existing null display driver). ARM64,
sound output and additional NT services remain outside the scope.

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
and selected original Wine WoW64 thunks with native unwind information.
No locally installed MinGW, copied
Wine installation, prebuilt DLL or absolute developer store path is an input.
The Linux Wine build supplies build tools and PE components. Native x64
unixlib DLLs are loaded from `x86_64-native` beside the x86 builtin DLLs;
the loader follows Wine's builtin/unixlib search and cache rules. The Civ3
renderer supplies its own x86_64-native unixlib. Wine's GUI unix side is
intended to run with the existing null display driver, without routing guest
user32 or gdi32 calls to the host's win32u.
The stock OpenGL PE also has a Wine-derived x64 null-device companion so its
process attach can complete before the full GUI stack is enabled.

`patches/` records changes to upstream Wine. `nix/` and the cross-build scripts
record compilation differences. The adapter sources under `prototype/` are
currently required port code, including Windows storage, thread and exception
integration. Extracted upstream routines and generated headers are build
outputs rather than separately maintained implementations.

Tests are a separate Nix output:

```sh
nix build '.#default^tests'
```

Their example native x64 bridge DLL and test EXEs are excluded from the runtime
output. `WINE_NATIVE_BRIDGE` can name an explicit native bridge DLL. Test runners
select their `native64.dll` fixture through that variable. Standard applications
run without a native bridge DLL. The optional `nix build .#corpus` downloads
hash-pinned unmodified applications. Copy that output to `prototype/unmodified`
beside the test runners. Mutable vendor URLs fail if their content no longer
matches the pinned hash.

Unmodified upstream Wine test sources have a separate x86 cross-build:

```sh
nix build .#wine-tests --out-link result-wine-upstream-tests
mkdir -p prototype/out/upstream-tests
cp -R --no-preserve=mode result-tests/prototype/out/. prototype/out/
cp -R --no-preserve=mode result-wine-upstream-tests/. prototype/out/upstream-tests/
```

On Windows, run `prototype/run-wine-tests.ps1`. It compares native Windows
with the port, records assertion summaries, raw output, numeric exits and
timeouts, and verifies each test executable against its source/build manifest.
The default selection covers ntdll routines and C++ runtime behavior.
The kernel32 fiber, heap, thread and synchronization suites are opt-in through
`-Tests`. They include cases that require excluded NT services or subprocess
support, so their failures need individual diagnosis. Failures unrelated to
GUI or excluded services take priority. The test build uses Wine's original
standalone runner and Windows debug helper without changing their sources.
See [upstream test results and priorities](docs/upstream-wine-tests.txt).

Windows VM execution is a separate validation step. Luna 6 runs those tests
from a frozen, separate Git worktree. The test bundle records the commit and
every deployed file's SHA256. Test runners and diagnostics are not runtime
dependencies. The standalone image-mapping and context-continuation helpers
are available through `nix build .#image-mapping-diagnostic` and
`nix build .#continuation-diagnostic`. `nix develop` supplies Python for source
and evidence helper scripts.

To prepare a frozen VM bundle directly from Nix outputs, commit the candidate
and run:

```sh
nix develop -c python3 prototype/bundle-vm-candidate.py \
  --out-directory result-tests/prototype/out \
  --upstream-directory result-wine-upstream-tests \
  --corpus-directory result-corpus
```

The helper requires a clean checkout and writes the archive and file manifest
under `evidence/`. The VM verifies the deployed files before execution.

This is still a port in development. Nix candidate `22ec829` passed the required
headless application, socket, APC, IOCP, callback, thread, context, XSTATE,
stack and fiber controls. Original Wine version and debug routines also pass.
See [the compatibility report](prototype/README.txt) and
[remaining work](prototype/REMAINING-WORK.txt) for the tested scope and limits.
The image-backed bootstrap also passes ten runtime cases and six unmodified
application commands with dynamic-code prohibition enabled. See
[the current regression report](docs/validation-22ec829.txt) for hashes,
accepted results and excluded harness runs. Earlier dynamic-code acceptance
is recorded in [the image-bootstrap report](docs/validation-d34a200.txt).
The [focused follow-up](docs/validation-d7eaf54.txt) verifies the corrected
context/I/O exit stress cases, canonical-address collisions and clean policy
failures.
The [wait-entry regression](docs/validation-5c02e38.txt) passes 100 runs after
removing the two demonstrated lock hazards. CET and arbitrary termination
during other adapter calls remain open. The interactive native-thread/window
fixture passes in Luna's logged-on session; the Civ3 smoke launch and the Wine
GUI/null-driver controls remain active work. Universal unmodified application
compatibility is not established.
