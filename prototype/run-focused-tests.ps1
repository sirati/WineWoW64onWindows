[CmdletBinding()]
param(
    [string[]]$Programs=@('thunk_fault32','unixlib_fault32','alignment32','thread_affinity32','process_view32','process_view_laa32','thread_priority32','selector_identity32','activation_context32','worker_process_exit32','threadpool_wait32','condition_wake32','tls_slots32','named_objects32','builtin_process32','memory_status32','xstate32','xstate_components32','thread_context32','thread_exit_context32','thread_delayed_context32','thread_create32','apc32','io_notify32','callback32','callback_worker32','fiber32','fiber_thread_exit32','fiber_overflow32','stack_growth32','stack_overflow32','io_thread_exit32'),
    [int]$Repeats=5,
    [string]$ReportName='focused-suites',
    [int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
if($Repeats -lt 1 -or $TimeoutSeconds -lt 1){throw 'Repeats and timeout must be positive'}
if($ReportName -notmatch '^[a-zA-Z0-9_-]+$'){throw 'Invalid report name'}
$root=$PSScriptRoot
$destination=Join-Path $root "out/results/$ReportName"
New-Item -ItemType Directory -Force $destination|Out-Null
$utf8=New-Object System.Text.UTF8Encoding($false)
$failures=0
foreach($program in $Programs) {
    if($program -notmatch '^[a-zA-Z0-9_]+$'){throw "Invalid fixture name: $program"}
    $fixtureReportName="$ReportName-$program"
    $fixtureReport=Join-Path $root "out/results/$fixtureReportName.json"
    Remove-Item -LiteralPath $fixtureReport -ErrorAction SilentlyContinue
    $runner=(Join-Path $root 'run-runtime-tests.ps1').Replace("'","''")
    $count=if($program -in @('thunk_fault32','thread_affinity32')){[Math]::Max(10,$Repeats)}else{$Repeats}
    $body="`$ErrorActionPreference='Stop'; `$global:LASTEXITCODE=0; & '$runner' -Programs '$program' -Repeats $count -ReportName '$fixtureReportName' -WineOnlyPrograms @('callback32','callback_worker32','fs_recovery32','fiber_thread_exit32','unixlib_fault32')"
    if($program -eq 'io_thread_exit32'){$body+=" -ProgramArguments @{'io_thread_exit32'='--spin'}"}
    $body+='; exit $LASTEXITCODE'
    $encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($body))
    $p=New-Object System.Diagnostics.Process
    try {
        $p.StartInfo.FileName="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
        $p.StartInfo.Arguments="-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encoded"
        $p.StartInfo.WorkingDirectory=$root
        $p.StartInfo.UseShellExecute=$false
        $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
        [void]$p.Start()
        $childPid=$p.Id
        $out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
        $timedOut=-not $p.WaitForExit($TimeoutSeconds*1000)
        if($timedOut) {
            & "$env:SystemRoot/System32/taskkill.exe" /PID $childPid /T /F|Out-Null
            if(-not $p.HasExited){$p.Kill()}
            $p.WaitForExit()
        }
        $exitCode=[int]$p.ExitCode
        # Emit the numeric exit before any serialization. Each finished child
        # gets its own record, so a later failure cannot discard earlier exits.
        Write-Output "SUITE_EXIT fixture=$program pid=$childPid code=$exitCode timedOut=$timedOut"
        $stdout=[string]$out.GetAwaiter().GetResult()
        $stderr=[string]$err.GetAwaiter().GetResult()
        $stdoutPath=Join-Path $destination "$program.stdout.txt"
        $stderrPath=Join-Path $destination "$program.stderr.txt"
        [IO.File]::WriteAllText($stdoutPath,$stdout,$utf8)
        [IO.File]::WriteAllText($stderrPath,$stderr,$utf8)
        $reportHash=if(Test-Path -LiteralPath $fixtureReport){(Get-FileHash -Algorithm SHA256 -LiteralPath $fixtureReport).Hash}else{''}
        $row=[pscustomobject]@{
            fixture=[string]$program;pid=[int]$childPid
            program=[string]$p.StartInfo.FileName;arguments=[string]$p.StartInfo.Arguments
            command=[string]$body;workingDirectory=[string]$root
            exitCode=$exitCode;timedOut=[bool]$timedOut;timeoutSeconds=[int]$TimeoutSeconds
            stdoutPath=[string]$stdoutPath;stderrPath=[string]$stderrPath
            fixtureReport=[string]$fixtureReport;fixtureReportSHA256=[string]$reportHash
        }
        $json=ConvertTo-Json -InputObject $row -Depth 4
        [IO.File]::WriteAllText((Join-Path $destination "$program.json"),$json,$utf8)
        if($timedOut -or $exitCode -ne 0 -or -not $reportHash){$failures++}
    } finally {$p.Dispose()}
}
Write-Output "FOCUSED_SUITE_FAILURES=$failures"
if($failures){exit 1}
