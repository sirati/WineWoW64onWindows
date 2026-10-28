[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$FixtureRoot,[int]$Repeats=5)
$ErrorActionPreference='Stop'
$runtime=(Resolve-Path "$PSScriptRoot/out").Path
$fixture=(Resolve-Path $FixtureRoot).Path
$destination=Join-Path $runtime 'results/unixlib-window'
New-Item -ItemType Directory -Force $destination|Out-Null
$utf8=New-Object System.Text.UTF8Encoding($false)
$rows=New-Object 'System.Collections.Generic.List[object]'
$oldBridge=$env:WINE_NATIVE_BRIDGE;$oldPath=$env:WINEDLLPATH;$oldOverrides=$env:WINEDLLOVERRIDES
Remove-Item Env:WINE_NATIVE_BRIDGE -ErrorAction SilentlyContinue
$env:WINEDLLPATH=$fixture;$env:WINEDLLOVERRIDES='wine_unixlib_window=b'
try {
    foreach($mode in @('wait','exit-live','worker-exit-live')) {
        foreach($native in @($true,$false)) {
            $count=if($native){1}else{$Repeats}
            for($i=0;$i -lt $count;$i++) {
                $name=if($native){"native-$mode-$i"}else{"wine-$mode-$i"}
                $p=New-Object System.Diagnostics.Process
                try {
                    $p.StartInfo.FileName=if($native){"$runtime/unixlib-window-native.exe"}else{"$runtime/wine-host.exe"}
                    $p.StartInfo.Arguments=if($native){'"'+"$fixture/x86_64-native/wine_unixlib_window.dll"+'" '+$mode}else{'--wine-components "'+"$fixture/wine-unixlib-window-app.exe"+'" '+$mode}
                    # Exercise runtime file lookup independently of guest CWD.
                    $p.StartInfo.WorkingDirectory=$fixture
                    $p.StartInfo.UseShellExecute=$false
                    $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
                    $started=[DateTime]::UtcNow
                    [void]$p.Start();$childPid=$p.Id
                    $out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
                    $limit=if($mode -eq 'wait'){90}else{45}
                    $timedOut=-not $p.WaitForExit($limit*1000)
                    if($timedOut){$p.Kill();$p.WaitForExit()}
                    $elapsed=([DateTime]::UtcNow-$started).TotalSeconds
                    $exit=[int]$p.ExitCode
                    $stdout=[string]$out.GetAwaiter().GetResult();$stderr=[string]$err.GetAwaiter().GetResult()
                    $prefix=if($native){'NATIVE_'}else{''}
                    $marker=switch($mode){
                        'wait' {'(?m)^'+$prefix+'WINDOW_OK mode=wait frames=([0-9]+) adapter=.+ \([^)]+\)\r?$'}
                        'exit-live' {'(?m)^'+$prefix+'WINDOW_EXIT_LIVE frames=([0-9]+) adapter=.+ \([^)]+\)\r?$'}
                        'worker-exit-live' {'(?m)^'+$prefix+'WINDOW_WORKER_EXIT frames=([0-9]+)\r?$'}
                    }
                    $markerMatches=$stdout -match $marker
                    $frames=if($markerMatches){[int]$Matches[1]}else{0}
                    $expected=if($mode -eq 'wait'){0}elseif($mode -eq 'exit-live'){37}else{38}
                    $load=$native -or $stdout -match 'WINE_LOAD_APPLICATION status=00000000'
                    $returned=$native -or $stdout -match "(?m)^GUEST_EXIT code=$expected .*HOST_RETURNED\r?$"
                    $minimum=if($mode -eq 'wait'){180}else{30}
                    $passed=-not $timedOut -and $exit -eq $expected -and $markerMatches -and $frames -ge $minimum -and $load -and $returned -and $stdout -notmatch 'WINDOW_FAILED'
                    $row=[pscustomobject]@{name=$name;native=$native;mode=$mode;iteration=$i;program=$p.StartInfo.FileName;arguments=$p.StartInfo.Arguments;workingDirectory=$fixture;wineDllPath=$env:WINEDLLPATH;wineDllOverrides=$env:WINEDLLOVERRIDES;pid=$childPid;exitCode=$exit;expectedExit=$expected;timedOut=$timedOut;timeoutSeconds=$limit;elapsedSeconds=$elapsed;marker=$marker;markerMatches=$markerMatches;frames=$frames;wineLoadMarker=$load;hostReturnMarker=$returned;passed=$passed;stdout=$stdout;stderr=$stderr}
                    $rows.Add($row)
                    [IO.File]::WriteAllText((Join-Path $destination "$name.json"),(ConvertTo-Json -InputObject $row -Depth 5),$utf8)
                    [IO.File]::WriteAllText((Join-Path $destination "$name.stdout.txt"),$stdout,$utf8)
                    [IO.File]::WriteAllText((Join-Path $destination "$name.stderr.txt"),$stderr,$utf8)
                    Write-Output "WINDOW_PROCESS name=$name pid=$childPid exit=$exit timedOut=$timedOut elapsed=$elapsed passed=$passed"
                } finally {$p.Dispose()}
            }
        }
    }
    [IO.File]::WriteAllText((Join-Path $destination 'processes.json'),(ConvertTo-Json -InputObject @($rows.ToArray()) -Depth 6),$utf8)
    $failures=@($rows|Where-Object {-not $_.passed}).Count
    Write-Output "WINDOW_FAILURES=$failures"
    if($failures){exit 1}
} finally {$env:WINE_NATIVE_BRIDGE=$oldBridge;$env:WINEDLLPATH=$oldPath;$env:WINEDLLOVERRIDES=$oldOverrides}
