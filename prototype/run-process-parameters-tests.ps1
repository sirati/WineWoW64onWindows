[CmdletBinding()]
param([int]$Repeats=5,[string]$BaselineHost='')
$ErrorActionPreference='Stop'
if($Repeats -lt 1){throw 'Repeats must be positive'}
$runtime=(Resolve-Path "$PSScriptRoot/out").Path
$destination=Join-Path $runtime 'results/process-parameters'
New-Item -ItemType Directory -Force $destination|Out-Null
$unicode=[string][char]0x03a9+'-'+[char]0xd83d+[char]0xde00
$fixtureDirectory=Join-Path $destination ('fixture-'+$unicode)
$workingDirectory=Join-Path $destination ('directory with spaces-'+$unicode)
New-Item -ItemType Directory -Force $fixtureDirectory,$workingDirectory|Out-Null
$source=Join-Path $runtime 'guest/process_parameters32.exe'
$fixture=Join-Path $fixtureDirectory 'process_parameters32.exe'
Copy-Item -LiteralPath $source -Destination $fixture -Force
$fixtureHash=(Get-FileHash -Algorithm SHA256 -LiteralPath $fixture).Hash
$testArguments='arg-'+$unicode+' "" "two words" "quote\"inside" "space slash\\"'
$testEnvironment='params-'+$unicode
$marker='process_parameters32: Unicode command line, environment, image, directory and standard handles passed'
$utf8=New-Object System.Text.UTF8Encoding($false)
$rows=New-Object 'System.Collections.Generic.List[object]'
$modes=@('native','wine')
if($BaselineHost){$BaselineHost=(Resolve-Path -LiteralPath $BaselineHost).Path;$modes+=@('baseline')}
foreach($mode in $modes) {
    $count=if($mode -eq 'wine'){$Repeats}else{1}
    for($i=0;$i -lt $count;$i++) {
        $name="$mode-$i"
        $p=New-Object System.Diagnostics.Process
        try {
            # The baseline uses an ASCII image path to reach the fixture's
            # argument check even before Unicode image lookup is implemented.
            $image=if($mode -eq 'baseline'){$source}else{$fixture}
            $p.StartInfo.FileName=if($mode -eq 'native'){$image}elseif($mode -eq 'baseline'){$BaselineHost}else{"$runtime/wine-host.exe"}
            $p.StartInfo.Arguments=if($mode -eq 'native'){$testArguments}else{'--wine-components "'+$image+'" '+$testArguments}
            $p.StartInfo.WorkingDirectory=$workingDirectory
            $p.StartInfo.UseShellExecute=$false
            $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
            $p.StartInfo.EnvironmentVariables['WINE_PROCESS_PARAMS_TEST']=$testEnvironment
            $p.StartInfo.EnvironmentVariables.Remove('WINE_NATIVE_BRIDGE')
            $started=[DateTime]::UtcNow
            [void]$p.Start();$childPid=$p.Id
            $out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
            $timedOut=-not $p.WaitForExit(30000)
            if($timedOut){$p.Kill();$p.WaitForExit()}
            $elapsed=([DateTime]::UtcNow-$started).TotalSeconds
            $exitCode=[int]$p.ExitCode
            $stdout=[string]$out.GetAwaiter().GetResult();$stderr=[string]$err.GetAwaiter().GetResult()
            $negative=$mode -eq 'baseline'
            $expected=if($negative){73}else{0}
            $expectedMarker=if($negative){'PROCESS_PARAMETERS_FAILED code=00000049'}else{$marker}
            $markerMatches=$stdout -match ('(?m)^'+[regex]::Escape($expectedMarker)+'\r?$')
            $load=$mode -eq 'native' -or $stdout -match 'WINE_LOAD_APPLICATION status=00000000'
            $passed=-not $timedOut -and $exitCode -eq $expected -and $markerMatches -and $load
            $row=[pscustomobject]@{name=$name;mode=$mode;negativeControl=$negative;program=$p.StartInfo.FileName;arguments=$p.StartInfo.Arguments;workingDirectory=$workingDirectory;fixture=$image;fixtureSHA256=$fixtureHash;environmentValue=$testEnvironment;pid=$childPid;exitCode=$exitCode;expectedExit=$expected;timedOut=$timedOut;timeoutSeconds=30;elapsedSeconds=$elapsed;marker=$expectedMarker;markerMatches=$markerMatches;wineLoadMarker=$load;passed=$passed;stdout=$stdout;stderr=$stderr}
            $rows.Add($row)
            [IO.File]::WriteAllText((Join-Path $destination "$name.json"),(ConvertTo-Json -InputObject $row -Depth 5),$utf8)
            [IO.File]::WriteAllText((Join-Path $destination "$name.stdout.txt"),$stdout,$utf8)
            [IO.File]::WriteAllText((Join-Path $destination "$name.stderr.txt"),$stderr,$utf8)
            Write-Output "PROCESS_PARAMETERS_PROCESS name=$name pid=$childPid exit=$exitCode timedOut=$timedOut passed=$passed"
        } finally {$p.Dispose()}
    }
}
[IO.File]::WriteAllText((Join-Path $destination 'processes.json'),(ConvertTo-Json -InputObject @($rows.ToArray()) -Depth 6),$utf8)
$failures=@($rows|Where-Object {-not $_.passed}).Count
Write-Output "PROCESS_PARAMETERS_FAILURES=$failures"
if($failures){exit 1}
