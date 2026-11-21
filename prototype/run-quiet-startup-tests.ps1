[CmdletBinding()]
param([string]$RuntimeRoot='', [string]$DllPath='', [string]$ReportName='quiet-startup')
$ErrorActionPreference='Stop'
$runtime=if($RuntimeRoot){(Resolve-Path $RuntimeRoot).Path}else{(Resolve-Path (Join-Path $PSScriptRoot 'out')).Path}
$results=Join-Path $runtime "results/$ReportName"
New-Item -ItemType Directory -Force $results|Out-Null
$utf8=New-Object Text.UTF8Encoding($false)
$rows=New-Object 'System.Collections.Generic.List[object]'
# Paired traced launches establish Wine's application-load marker. Quiet
# launches use the same fixture hash, numeric exit and exact file marker.
# A guest that writes to the console is deliberately not used for silence checks.
foreach($fixture in @('quiet32','opengl_import32')){
    $exe=Join-Path $runtime "guest/$fixture.exe"
    $hash=(Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash
    $expected=if($fixture -eq 'quiet32'){"QUIET_OK`n"}else{"OPENGL_IMPORT_OK`n"}
    $cases=@(@{name='native';native=$true;trace=$false;external=$true},
             @{name='quiet-runtime';native=$false;trace=$false;external=$false},
             @{name='quiet-external';native=$false;trace=$false;external=$true},
             @{name='trace-external';native=$false;trace=$true;external=$true})
    foreach($case in $cases){
        $name="$fixture-$($case.name)";$cwd=if($case.external){Join-Path $results $name}else{$runtime}
        New-Item -ItemType Directory -Force $cwd|Out-Null
        $markerFile=Join-Path $cwd "$fixture.ok"
        Remove-Item -LiteralPath $markerFile -ErrorAction SilentlyContinue
        $p=New-Object Diagnostics.Process
        try{
            $p.StartInfo.FileName=if($case.native){$exe}else{Join-Path $runtime 'wine-host.exe'}
            $p.StartInfo.Arguments=if($case.native){''}else{'--wine-components "'+$exe+'"'}
            $p.StartInfo.WorkingDirectory=$cwd;$p.StartInfo.UseShellExecute=$false
            $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
            foreach($variable in @('WINEDEBUG','WINE_PROBE_TRACE','PROBE_WINE_DEBUG','WINE_NATIVE_BRIDGE','WINEDLLPATH','WINEDLLOVERRIDES')){[void]$p.StartInfo.EnvironmentVariables.Remove($variable)}
            if($DllPath -and -not $case.native){$p.StartInfo.EnvironmentVariables['WINEDLLPATH']=$DllPath}
            if($case.trace){$p.StartInfo.EnvironmentVariables['WINEDEBUG']='trace+winehost,trace+module'}
            [void]$p.Start();$null=$p.Handle;$processId=$p.Id
            $out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
            $timedOut=-not $p.WaitForExit(30000)
            if($timedOut){& "$env:SystemRoot/System32/taskkill.exe" /PID $processId /T /F|Out-Null;if(-not $p.HasExited){$p.Kill()};$p.WaitForExit()}
            $stdout=[string]$out.GetAwaiter().GetResult();$stderr=[string]$err.GetAwaiter().GetResult()
            $marker=if(Test-Path -LiteralPath $markerFile){[IO.File]::ReadAllText($markerFile)}else{''}
            $load=($stdout+"`n"+$stderr) -match 'WINE_LOAD_APPLICATION status=00000000'
            $quiet=($stdout.Length -eq 0 -and $stderr.Length -eq 0)
            $passed=(-not $timedOut -and $p.ExitCode -eq 0 -and $marker -ceq $expected -and $(if($case.trace){$load}else{$quiet}))
            $row=[pscustomobject]@{name=$name;fixture=$exe;fixtureSHA256=$hash;program=$p.StartInfo.FileName;arguments=$p.StartInfo.Arguments;workingDirectory=$cwd;native=$case.native;trace=$case.trace;wineDebug=$p.StartInfo.EnvironmentVariables['WINEDEBUG'];wineDllPath=$p.StartInfo.EnvironmentVariables['WINEDLLPATH'];pid=$processId;exitCode=[int]$p.ExitCode;timedOut=$timedOut;timeoutSeconds=30;markerPath=$markerFile;expectedMarker=$expected;actualMarker=$marker;wineLoadMarker=$load;quiet=$quiet;passed=$passed;stdout=$stdout;stderr=$stderr}
            $rows.Add($row)
            [IO.File]::WriteAllText((Join-Path $results "$name.json"),($row|ConvertTo-Json -Depth 6),$utf8)
            [IO.File]::WriteAllText((Join-Path $results "$name.stdout.txt"),$stdout,$utf8)
            [IO.File]::WriteAllText((Join-Path $results "$name.stderr.txt"),$stderr,$utf8)
            Write-Output "QUIET_STARTUP name=$name pid=$processId exit=$($row.exitCode) timedOut=$timedOut quiet=$quiet passed=$passed"
        }finally{$p.Dispose();Remove-Item -LiteralPath $markerFile -ErrorAction SilentlyContinue}
    }
}
$failures=@($rows|Where-Object {-not $_.passed}).Count
[IO.File]::WriteAllText((Join-Path $results 'report.json'),([pscustomobject]@{rows=$rows.ToArray();failures=$failures}|ConvertTo-Json -Depth 8),$utf8)
Write-Output "QUIET_STARTUP_FAILURES=$failures"
if($failures){exit 1}
exit 0
