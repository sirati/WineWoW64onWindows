[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$GameRoot,
    [int]$Seconds=30,
    [string]$ReportName='civ3-smoke',
    [string]$RuntimeRoot='',
    [string]$WineDebug='',
    [string]$GuestDebug=''
)
$ErrorActionPreference='Stop'
if($Seconds -lt 1 -or $Seconds -gt 600){throw 'Seconds must be between 1 and 600'}
$root=$PSScriptRoot
$runtime=if($RuntimeRoot){(Resolve-Path $RuntimeRoot).Path}
         elseif(Test-Path -LiteralPath (Join-Path $root 'runtime/wine-host.exe')){(Resolve-Path (Join-Path $root 'runtime')).Path}
         else{(Resolve-Path $root).Path}
$game=(Resolve-Path $GameRoot).Path
$application=Join-Path $game 'Conquests/Civ3Conquests.exe'
if(-not (Test-Path -LiteralPath $application)){throw "Missing Civ3Conquests.exe: $application"}
$results=Join-Path $runtime "results/$ReportName"
New-Item -ItemType Directory -Force $results|Out-Null
$utf8=New-Object System.Text.UTF8Encoding($false)
$program=Join-Path $runtime 'wine-host.exe'
$arguments='--wine-components "'+$application+'"'
$p=New-Object System.Diagnostics.Process
try {
    $p.StartInfo.FileName=$program
    $p.StartInfo.Arguments=$arguments
    $p.StartInfo.WorkingDirectory=(Join-Path $game 'Conquests')
    $p.StartInfo.UseShellExecute=$false
    $p.StartInfo.CreateNoWindow=$true
    $p.StartInfo.RedirectStandardOutput=$true
    $p.StartInfo.RedirectStandardError=$true
    # Host-side Wine code (win32u, server) and the x86 Wine DLLs log separately.
    if($WineDebug){$p.StartInfo.EnvironmentVariables['PROBE_WINE_DEBUG']=$WineDebug}
    if($GuestDebug){$p.StartInfo.EnvironmentVariables['WINEDEBUG']=$GuestDebug}
    [void]$p.Start();$pidValue=$p.Id;$sessionId=$p.SessionId
    $outTask=$p.StandardOutput.ReadToEndAsync();$errTask=$p.StandardError.ReadToEndAsync()
    $timedOut=-not $p.WaitForExit($Seconds*1000)
    if($timedOut){
        & "$env:SystemRoot/System32/taskkill.exe" /PID $pidValue /T /F|Out-Null
        if(-not $p.HasExited){$p.Kill()};$p.WaitForExit()
    }
    $exitCode=[int]$p.ExitCode
    $stdout=[string]$outTask.GetAwaiter().GetResult()
    $stderr=[string]$errTask.GetAwaiter().GetResult()
    $row=[pscustomobject]@{
        name=$ReportName;program=$program;arguments=$arguments
        workingDirectory=$p.StartInfo.WorkingDirectory;pid=$pidValue;sessionId=$sessionId
        timeoutSeconds=$Seconds;exitCode=$exitCode;timedOut=$timedOut
        wineDebug=$WineDebug;guestDebug=$GuestDebug
        wineLoadMarker=($stdout -match 'WINE_LOAD_APPLICATION status=00000000')
        wineImportsMarker=($stdout -match 'WINE_IMPORTS status=00000000')
        wineAttachMarker=($stdout -match 'WINE_ATTACH status=00000000')
        stdout=$stdout;stderr=$stderr
    }
    [IO.File]::WriteAllText((Join-Path $results "$ReportName.json"),(ConvertTo-Json $row -Depth 6),$utf8)
    [IO.File]::WriteAllText((Join-Path $results "$ReportName.stdout.txt"),$stdout,$utf8)
    [IO.File]::WriteAllText((Join-Path $results "$ReportName.stderr.txt"),$stderr,$utf8)
    $row|ConvertTo-Json -Depth 6|Write-Output
    if($timedOut -and $row.wineLoadMarker -and $row.wineImportsMarker -and $row.wineAttachMarker){exit 0}
    if($timedOut){exit 124}
    exit $exitCode
} finally {$p.Dispose()}
