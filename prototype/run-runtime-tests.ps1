[CmdletBinding()]
param([string[]]$Programs=@('socket_dns32'),[int]$Repeats=1,[string[]]$WineOnlyPrograms=@('callback32','callback_worker32','fs_recovery32'),[hashtable]$ProgramArguments=@{},[string]$ReportName='runtime-integration-report',[switch]$StopOnFailure)
$ErrorActionPreference='Stop'
Set-Location "$PSScriptRoot/out"
$env:WINE_NATIVE_BRIDGE="native64.dll"
New-Item -ItemType Directory -Force results|Out-Null
$rows=@()
foreach($program in $Programs) {
    $guestArguments=if($ProgramArguments.ContainsKey($program)){[string]$ProgramArguments[$program]}else{''}
    $modes=if($program -in $WineOnlyPrograms){@($false)}else{@($true,$false)}
    foreach($native in $modes) {
        $count=if($native){1}else{$Repeats}
        for($i=0;$i -lt $count;$i++) {
            $p=New-Object System.Diagnostics.Process
            $p.StartInfo.FileName=if($native){(Resolve-Path "guest/$program.exe").Path}else{(Resolve-Path 'wine-host.exe').Path}
            $p.StartInfo.Arguments=if($native){$guestArguments}else{"--wine-components guest/$program.exe $guestArguments"}
            $p.StartInfo.WorkingDirectory=(Get-Location).Path
            $p.StartInfo.UseShellExecute=$false;$p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
            [void]$p.Start();$out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
            $timeout=-not $p.WaitForExit(15000)
            if($timeout){$p.Kill();$p.WaitForExit()}
            $pattern=switch($program){
                'stack_growth32' { 'stack_growth32:.*guard pages.*successfully' }
                'thread_delayed_context32' { '(?m)^thread_delayed_context32: normal and forced exit retain valid context-handle errors after cleanup passed\r?$' }
                'fs_recovery32' { 'fs_recovery32:.*recovered and retried' }
                'stack_overflow32' { 'stack_overflow32:.*repeated growth passed' }
                default { "$program`:.*passed" }
            }
            $modeMatches=$true
            if($program -eq 'io_thread_exit32' -and $guestArguments -match '(^|\s)--spin($|\s)') {
                $modeMatches=$out.Result -match 'spin-issuer mode:\s*00000001'
            }
            $loaderMatches=$native -or ($out.Result -match 'WINE_LOAD_APPLICATION status=00000000' -and
                                       $out.Result -match 'GUEST_EXIT code=0 .*HOST_RETURNED')
            $rows+=[pscustomobject]@{probe=$program;native=$native;iteration=$i;pid=$p.Id;program=$p.StartInfo.FileName;arguments=$p.StartInfo.Arguments;workingDirectory=$p.StartInfo.WorkingDirectory;exitCode=$p.ExitCode;timedOut=$timeout;passed=(-not $timeout -and $p.ExitCode -eq 0 -and $out.Result -match $pattern -and $modeMatches -and $loaderMatches);stdout=$out.Result;stderr=$err.Result}
            $p.Dispose()
            if($StopOnFailure -and -not $rows[-1].passed){break}
        }
        if($StopOnFailure -and -not $rows[-1].passed){break}
    }
    if($StopOnFailure -and -not $rows[-1].passed){break}
}
$rows|ConvertTo-Json -Depth 6|Set-Content -Encoding UTF8 "results/$ReportName.json"
$rows|Select-Object probe,native,iteration,exitCode,timedOut,passed|Format-Table -AutoSize
if(@($rows|Where-Object {-not $_.passed}).Count){$rows|Where-Object {-not $_.passed}|Format-List;exit 1}
