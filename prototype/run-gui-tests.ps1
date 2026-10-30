[CmdletBinding()]
param(
    [string[]]$Modes=@('window','timer','cursor','dc','focus','thread','text','msgtime','opengl','comthread'),
    [int]$Repeats=3,
    [string]$ReportName='gui-controls',
    [int]$TimeoutSeconds=30,
    [string]$WineDebug='',
    [string]$GuestDebug='',
    [switch]$SkipNative,
    # Another wine-host.exe in out/ (it uses out/'s runtime), e.g. an earlier
    # candidate's host as a negative control.
    [string]$HostExe=''
)
# Wine's own win32u with the null display driver, against the same EXE run
# natively. Each Wine row must print GUI_OK for its mode, the Wine
# application-load marker and the host return marker, and exit 0.
$ErrorActionPreference='Stop'
if($Repeats -lt 1 -or $TimeoutSeconds -lt 1){throw 'Repeats and timeout must be positive'}
if($ReportName -notmatch '^[a-zA-Z0-9_-]+$'){throw 'Invalid report name'}
$out=Join-Path $PSScriptRoot 'out'
$results=Join-Path $out 'results'
New-Item -ItemType Directory -Force $results|Out-Null
$utf8=New-Object System.Text.UTF8Encoding($false)
$guest=(Resolve-Path (Join-Path $out 'guest/gui_controls32.exe')).Path
$wineHost=(Resolve-Path $(if($HostExe){$HostExe}else{Join-Path $out 'wine-host.exe'})).Path
$rows=@()
foreach($mode in $Modes) {
    if($mode -notmatch '^[a-z]+$'){throw "Invalid mode: $mode"}
    $runs=@()
    if(-not $SkipNative){$runs+=,@($true,0)}
    for($i=0;$i -lt $Repeats;$i++){$runs+=,@($false,$i)}
    foreach($run in $runs) {
        $native=$run[0];$iteration=$run[1]
        $p=New-Object System.Diagnostics.Process
        try {
            $p.StartInfo.FileName=if($native){$guest}else{$wineHost}
            $p.StartInfo.Arguments=if($native){$mode}else{"--wine-components guest/gui_controls32.exe $mode"}
            $p.StartInfo.WorkingDirectory=$out
            $p.StartInfo.UseShellExecute=$false
            $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
            # Host-side Wine code (win32u, server) and the x86 Wine DLLs log separately.
            if($WineDebug -and -not $native){$p.StartInfo.EnvironmentVariables['PROBE_WINE_DEBUG']=$WineDebug}
            if($GuestDebug -and -not $native){$p.StartInfo.EnvironmentVariables['WINEDEBUG']=$GuestDebug}
            [void]$p.Start()
            $childPid=$p.Id;$sessionId=$p.SessionId
            $outTask=$p.StandardOutput.ReadToEndAsync();$errTask=$p.StandardError.ReadToEndAsync()
            $timedOut=-not $p.WaitForExit($TimeoutSeconds*1000)
            if($timedOut) {
                & "$env:SystemRoot/System32/taskkill.exe" /PID $childPid /T /F|Out-Null
                if(-not $p.HasExited){$p.Kill()}
                $p.WaitForExit()
            }
            $exitCode=[int]$p.ExitCode
            Write-Output "GUI_PROCESS mode=$mode native=$native iteration=$iteration pid=$childPid exit=$exitCode timedOut=$timedOut"
            $stdout=[string]$outTask.GetAwaiter().GetResult()
            $stderr=[string]$errTask.GetAwaiter().GetResult()
            $marker="(?m)^GUI_OK mode=$mode .+\r?$"
            $markerMatches=$stdout -match $marker
            $loadMarker=$native -or $stdout -match 'WINE_LOAD_APPLICATION status=00000000'
            $hostReturn=$native -or $stdout -match '(?m)^GUEST_EXIT code=0 .*HOST_RETURNED'
            $name=if($native){"native-$mode"}else{"wine-$mode-$iteration"}
            $rows+=[pscustomobject]@{
                name=$name;mode=$mode;native=$native;iteration=$iteration
                program=$p.StartInfo.FileName;arguments=$p.StartInfo.Arguments;workingDirectory=$out
                wineDebug=if($native){''}else{$WineDebug};guestDebug=if($native){''}else{$GuestDebug}
                sessionId=$sessionId;userInteractive=[Environment]::UserInteractive
                pid=$childPid;exitCode=$exitCode;expectedExit=0;timedOut=$timedOut;timeoutSeconds=$TimeoutSeconds
                marker=$marker;markerMatches=$markerMatches;wineLoadMarker=$loadMarker;hostReturnMarker=$hostReturn
                passed=(-not $timedOut -and $exitCode -eq 0 -and $markerMatches -and $loadMarker -and $hostReturn)
                stdout=$stdout;stderr=$stderr
            }
            [IO.File]::WriteAllText((Join-Path $results "$ReportName-$name.stdout.txt"),$stdout,$utf8)
            [IO.File]::WriteAllText((Join-Path $results "$ReportName-$name.stderr.txt"),$stderr,$utf8)
        } finally {$p.Dispose()}
    }
}
[IO.File]::WriteAllText((Join-Path $results "$ReportName.json"),(ConvertTo-Json -InputObject $rows -Depth 4),$utf8)
$rows|Select-Object name,exitCode,timedOut,markerMatches,passed|Format-Table -AutoSize|Out-String|Write-Output
$failed=@($rows|Where-Object {-not $_.passed}).Count
Write-Output "GUI_CONTROL_FAILURES=$failed"
if($failed){exit 1}
exit 0
