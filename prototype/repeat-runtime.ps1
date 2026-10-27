$ErrorActionPreference='Stop'
Set-Location "$PSScriptRoot/out"
$env:WINE_NATIVE_BRIDGE="native64.dll"
$rows=@()
foreach($guest in @('concurrent32','teb32','bridge_app32','crt32')) {
    for($i=0;$i -lt 30;$i++) {
        $p=New-Object System.Diagnostics.Process
        $p.StartInfo.FileName=(Resolve-Path './wine-host.exe').Path
        $p.StartInfo.WorkingDirectory=(Get-Location).Path
        $p.StartInfo.Arguments="--wine-components guest/$guest.exe"
        $p.StartInfo.UseShellExecute=$false;$p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
        [void]$p.Start();$out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
        $timeout=-not $p.WaitForExit(10000)
        if($timeout){$p.Kill();$p.WaitForExit()}
        $text=$out.Result
        $repairs=0;if($text -match 'fs_repairs=(\d+)'){$repairs=[int]$Matches[1]}
        $rows+=[pscustomobject]@{guest=$guest;iteration=$i;exitCode=$p.ExitCode;timedOut=$timeout;fsRepairs=$repairs;stdout=$text;stderr=$err.Result}
        $p.Dispose()
    }
}
$rows|ConvertTo-Json -Depth 5|Set-Content -Encoding UTF8 results/runtime-repeat.json
$rows|Group-Object guest|ForEach-Object {
    $bad=@($_.Group|Where-Object {$_.exitCode -ne 0 -or $_.timedOut})
    Write-Output "$($_.Name): $($_.Count) runs, $($bad.Count) failures, $(($_.Group|Measure-Object -Property fsRepairs -Sum).Sum) FS repairs"
}
if(@($rows|Where-Object {$_.exitCode -ne 0 -or $_.timedOut}).Count){exit 1}
