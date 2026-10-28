[CmdletBinding()]
param([int]$Repeats=1,[switch]$PipeControls)
$ErrorActionPreference='Stop'
Set-Location "$PSScriptRoot/out"
$env:WINE_NATIVE_BRIDGE="native64.dll"
New-Item -ItemType Directory -Force results | Out-Null
function New-Process($program,$arguments) {
    $p=New-Object System.Diagnostics.Process
    $p.StartInfo.FileName=(Resolve-Path $program).Path
    $p.StartInfo.WorkingDirectory=(Get-Location).Path
    $p.StartInfo.Arguments=$arguments
    $p.StartInfo.UseShellExecute=$false;$p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
    [void]$p.Start();return $p
}
$rows=@()
$peerArguments=if($PipeControls){'--pipe-control'}else{''}
$guestExe=if($PipeControls){'guest/pipe_control32.exe'}else{'guest/async_event32.exe'}
$reportName=if($PipeControls){'pipe-control-report.json'}else{'async-report.json'}
$expectedMessage=if($PipeControls){'pipe_control32:.*passed'}else{'async_event32:.*passed'}
Remove-Item "results/$reportName" -ErrorAction SilentlyContinue
foreach($native in @($true,$false)) {
    $count=if($native){1}else{$Repeats}
    for($i=0;$i -lt $count;$i++) {
        $peer=$null;$guest=$null
        try {
            $peer=New-Process './async-peer64.exe' $peerArguments
            $peerError=$peer.StandardError.ReadToEndAsync()
            $ready=$peer.StandardOutput.ReadLineAsync()
            if(-not $ready.Wait(3000) -or $ready.Result -ne 'peer ready'){throw 'I/O peer did not initialize'}
            $peerOutput=$peer.StandardOutput.ReadToEndAsync()
            $guest=if($native){New-Process "./$guestExe" ''}else{New-Process './wine-host.exe' "--wine-components $guestExe"}
            $out=$guest.StandardOutput.ReadToEndAsync();$err=$guest.StandardError.ReadToEndAsync()
            $timeout=-not $guest.WaitForExit(10000)
            if($timeout){$guest.Kill();$guest.WaitForExit()}
            $peerTimeout=-not $peer.WaitForExit(3000)
            if($peerTimeout){$peer.Kill();$peer.WaitForExit()}
            $loaderMatches=$native -or $out.Result -match 'WINE_LOAD_APPLICATION status=00000000'
            $rows+=[pscustomobject]@{probe=$guestExe;pid=$guest.Id;program=$guest.StartInfo.FileName;arguments=$guest.StartInfo.Arguments;workingDirectory=$guest.StartInfo.WorkingDirectory;peerPid=$peer.Id;peerProgram=$peer.StartInfo.FileName;peerArguments=$peer.StartInfo.Arguments;native=$native;iteration=$i;exitCode=$guest.ExitCode;peerExitCode=$peer.ExitCode;timedOut=($timeout -or $peerTimeout);guestTimedOut=$timeout;peerTimedOut=$peerTimeout;passed=(-not $timeout -and -not $peerTimeout -and $guest.ExitCode -eq 0 -and $peer.ExitCode -eq 0 -and $out.Result -match $expectedMessage -and $loaderMatches);stdout=$out.Result;stderr=$err.Result;peerStdout=($ready.Result+"`r`n"+$peerOutput.Result);peerStderr=$peerError.Result}
        } finally {
            foreach($p in @($guest,$peer)){if($p){if(-not $p.HasExited){$p.Kill();$p.WaitForExit()};$p.Dispose()}}
        }
    }
}
$rows|ConvertTo-Json -Depth 5|Set-Content -Encoding UTF8 "results/$reportName"
$rows|Select-Object native,iteration,exitCode,peerExitCode,timedOut,passed|Format-Table -AutoSize
if(@($rows|Where-Object {-not $_.passed}).Count){$rows|Where-Object {-not $_.passed}|Format-List;exit 1}
