# Port constraints

Keep application and DLL loading in Wine. Reuse Wine's algorithms and keep
source changes small. GUI and sound integration are excluded. The current
goal includes additional guest NT services needed by non-GUI, non-audio tests.
The runtime target remains x86 guests in an x64 Windows host.
The repository records the pinned Nix cross build, patches, Windows adapters,
test helpers and documentation. Generated Wine sources and binaries stay out
of Git. Windows runs tests; Linux/Nix/MinGW builds the runtime and test inputs.

# VM tests

The user requires Luna 6 for VM execution. Luna may write test helpers but
must not change production code. The primary must create and populate a clean
frozen worktree before delegation. Work on an independent task during testing.

Use the local `/home/sirati/winvm/bin/winvm` CLI. Verify the candidate manifest
in the VM before testing. Keep raw output, actual arguments, numeric process
exit codes, timeouts, native controls and negative controls. Retrieve evidence
and destroy the test VM afterward.

Never name a PowerShell function's argument parameter `$args`; it is reserved
and can silently drop supplied arguments. Use `$arguments`. Launch suite
runners as direct PowerShell child processes to capture their real exit code.
For a fixture to pass, require its exact success marker, the intended actual
arguments, exit code and timeout result. Wine runs must also have the Wine
application-load marker. Inspect raw evidence before accepting an agent's
summary. A labeled row or exit 0 alone cannot establish which program ran.

Prioritize upstream failures outside the excluded areas. An unsupported-service
message alone does not establish the cause of nearby assertions. Report
completed suites separately from assertions reached before a crash.

# Publication

Publish authorized work to `git@github.com:sirati/WineWoW64onWindows.git` over
SSH using the 1Password agent at `/home/sirati/.1password/agent.sock`.
