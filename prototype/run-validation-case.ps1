[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Name)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
& "$root/verify-candidate.ps1" -Root $root
$cases=Get-Content -Raw -LiteralPath "$root/validation-cases.json"|ConvertFrom-Json
$selected=@($cases|Where-Object {$_.name -eq $Name})
if($selected.Count -ne 1){throw "Unknown or repeated validation case: $Name"}
$case=$selected[0]
if($Name -notmatch '^[a-zA-Z0-9_-]+$'){throw 'Invalid case name'}
$destination=Join-Path $root 'suite-records'
New-Item -ItemType Directory -Force $destination|Out-Null
$record=Join-Path $destination "$Name.json"
if(Test-Path -LiteralPath $record){throw "Preserve existing case evidence: $record"}
$utf8=New-Object System.Text.UTF8Encoding($false)
$p=New-Object System.Diagnostics.Process
try {
    $p.StartInfo.FileName="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
    $script=(Join-Path $root ([string]$case.script)).Replace("'","''")
    $body="`$ErrorActionPreference='Stop'; `$global:LASTEXITCODE=0; & '$script' $($case.arguments); exit `$LASTEXITCODE"
    $encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($body))
    $p.StartInfo.Arguments="-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encoded"
    $p.StartInfo.WorkingDirectory=$root
    $p.StartInfo.UseShellExecute=$false
    $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
    [void]$p.Start();$childPid=$p.Id;$sessionId=$p.SessionId
    $out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
    $timedOut=-not $p.WaitForExit([int]$case.timeoutSeconds*1000)
    if($timedOut){
        & "$env:SystemRoot/System32/taskkill.exe" /PID $childPid /T /F|Out-Null
        if(-not $p.HasExited){$p.Kill()};$p.WaitForExit()
    }
    $exitCode=[int]$p.ExitCode
    Write-Output "SUITE_EXIT name=$Name pid=$childPid code=$exitCode timedOut=$timedOut"
    $stdout=[string]$out.GetAwaiter().GetResult();$stderr=[string]$err.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $destination "$Name.stdout.txt"),$stdout,$utf8)
    [IO.File]::WriteAllText((Join-Path $destination "$Name.stderr.txt"),$stderr,$utf8)
    $row=[pscustomobject]@{name=$Name;program=$p.StartInfo.FileName;arguments=$p.StartInfo.Arguments;command=$body;workingDirectory=$root;pid=$childPid;sessionId=$sessionId;exitCode=$exitCode;timedOut=$timedOut;timeoutSeconds=[int]$case.timeoutSeconds;stdout=$stdout;stderr=$stderr}
    $json=ConvertTo-Json -InputObject $row -Depth 5
    [IO.File]::WriteAllText($record,$json,$utf8)
    # Duplicate completed evidence into the host's SSH record before the next
    # suite. A later VM failure must not erase a completed process's report.
    Write-Output $json
    foreach($report in $case.reports){
        $path=Join-Path $root ([string]$report)
        if(Test-Path -LiteralPath $path){
            Write-Output "BEGIN_REPORT $report"
            Get-Content -Raw -LiteralPath $path|Write-Output
            Write-Output "END_REPORT $report"
        }
    }
    & "$root/verify-candidate.ps1" -Root $root
    if($timedOut){exit 124}
    exit $exitCode
} finally {$p.Dispose()}
