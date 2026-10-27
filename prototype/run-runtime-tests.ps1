[CmdletBinding()]
param([string[]]$Programs=@('socket_dns32'),[int]$Repeats=1,[string[]]$WineOnlyPrograms=@('callback32','callback_worker32','fs_recovery32'),[switch]$StopOnFailure)
$ErrorActionPreference='Stop'
Set-Location "$PSScriptRoot/out"
$env:WINE_NATIVE_BRIDGE="native64.dll"
New-Item -ItemType Directory -Force results|Out-Null
$rows=@()
foreach($program in $Programs) {
    $modes=if($program -in $WineOnlyPrograms){@($false)}else{@($true,$false)}
    foreach($native in $modes) {
        $count=if($native){1}else{$Repeats}
        for($i=0;$i -lt $count;$i++) {
            $p=New-Object System.Diagnostics.Process
            $p.StartInfo.FileName=if($native){(Resolve-Path "guest/$program.exe").Path}else{(Resolve-Path 'wine-host.exe').Path}
            $p.StartInfo.Arguments=if($native){''}else{"--wine-components guest/$program.exe"}
            $p.StartInfo.WorkingDirectory=(Get-Location).Path
            $p.StartInfo.UseShellExecute=$false;$p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
            [void]$p.Start();$out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
            $timeout=-not $p.WaitForExit(15000)
            if($timeout){$p.Kill();$p.WaitForExit()}
            $pattern=switch($program){
                'stack_growth32' { 'stack_growth32:.*guard pages.*successfully' }
                'fs_recovery32' { 'fs_recovery32:.*recovered and retried' }
                'stack_overflow32' { 'stack_overflow32:.*repeated growth passed' }
                default { "$program`:.*passed" }
            }
            $rows+=[pscustomobject]@{probe=$program;native=$native;iteration=$i;exitCode=$p.ExitCode;timedOut=$timeout;passed=(-not $timeout -and $p.ExitCode -eq 0 -and $out.Result -match $pattern);stdout=$out.Result;stderr=$err.Result}
            $p.Dispose()
            if($StopOnFailure -and -not $rows[-1].passed){break}
        }
        if($StopOnFailure -and -not $rows[-1].passed){break}
    }
    if($StopOnFailure -and -not $rows[-1].passed){break}
}
$rows|ConvertTo-Json -Depth 6|Set-Content -Encoding UTF8 results/runtime-integration-report.json
$rows|Select-Object probe,native,iteration,exitCode,timedOut,passed|Format-Table -AutoSize
if(@($rows|Where-Object {-not $_.passed}).Count){$rows|Where-Object {-not $_.passed}|Format-List;exit 1}
