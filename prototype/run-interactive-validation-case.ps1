[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Name)
$ErrorActionPreference='Stop'
if($Name -notmatch '^[a-zA-Z0-9_-]+$'){throw 'Invalid case name'}
$root=$PSScriptRoot
& "$root/verify-candidate.ps1" -Root $root
$selected=@((Get-Content -Raw "$root/validation-cases.json"|ConvertFrom-Json)|Where-Object {$_.name -eq $Name})
if($selected.Count -ne 1){throw 'Unknown interactive case'}
$case=$selected[0]
$destination=Join-Path $root 'interactive-records'
New-Item -ItemType Directory -Force $destination|Out-Null
$identity=[Security.Principal.WindowsIdentity]::GetCurrent().Name
$taskName='WineWoW64-'+$Name+'-'+[guid]::NewGuid().ToString('N')
$utf8=New-Object System.Text.UTF8Encoding($false)
$launch=[ordered]@{name=$Name;user=$identity;callerSessionId=[Diagnostics.Process]::GetCurrentProcess().SessionId;taskName=$taskName;startedUTC=[DateTime]::UtcNow.ToString('o')}
$registered=$false
try {
    try {$launch.sessions=(& "$env:SystemRoot/System32/quser.exe" 2>&1|Out-String)}
    catch {$launch.sessionsError=$_.Exception.Message}
    $principal=New-ScheduledTaskPrincipal -UserId $identity -LogonType Interactive -RunLevel Highest
    $action=New-ScheduledTaskAction -Execute "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -WorkingDirectory $root -Argument ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "'+"$root/run-validation-case.ps1"+'" -Name '+$Name)
    Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal|Out-Null
    $registered=$true
    Export-ScheduledTask -TaskName $taskName|Set-Content -Encoding UTF8 (Join-Path $destination "$Name.task.xml")
    Start-ScheduledTask -TaskName $taskName
    $began=[DateTime]::UtcNow
    $started=$false;$completed=$false
    while(([DateTime]::UtcNow-$began).TotalSeconds -lt ([int]$case.timeoutSeconds+60)) {
        $task=Get-ScheduledTask -TaskName $taskName
        if($task.State -eq 'Running'){$started=$true}
        if((Test-Path -LiteralPath "$root/suite-records/$Name.json") -and $task.State -ne 'Running'){$completed=$true;break}
        if(-not $started -and ([DateTime]::UtcNow-$began).TotalSeconds -ge 30){break}
        Start-Sleep -Seconds 1
    }
    $info=Get-ScheduledTaskInfo -TaskName $taskName
    $launch.taskStarted=$started;$launch.completed=$completed
    $launch.taskResult=[long]$info.LastTaskResult
    $launch.lastRunTime=[string]$info.LastRunTime
    $launch.finishedUTC=[DateTime]::UtcNow.ToString('o')
    if(-not $completed){
        Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
        $launch.prerequisite='No completed interactive task; inspect logged-on sessions and task result'
        Write-Output (ConvertTo-Json -InputObject $launch -Depth 5)
        exit 125
    }
    $record=Get-Content -Raw -LiteralPath "$root/suite-records/$Name.json"|ConvertFrom-Json
    $launch.targetSessionId=$record.sessionId
    $launch.interactiveSession=$record.sessionId -gt 0
    Write-Output (ConvertTo-Json -InputObject $launch -Depth 5)
    Get-Content -Raw -LiteralPath "$root/suite-records/$Name.json"|Write-Output
    foreach($report in $case.reports){
        $path=Join-Path $root ([string]$report)
        if(Test-Path -LiteralPath $path){
            Write-Output "BEGIN_REPORT $report";Get-Content -Raw -LiteralPath $path|Write-Output;Write-Output "END_REPORT $report"
        }
    }
    if(-not $launch.interactiveSession){exit 125}
    if($record.timedOut){exit 124}
    exit ([int]$record.exitCode)
} finally {
    [IO.File]::WriteAllText((Join-Path $destination "$Name.launch.json"),(ConvertTo-Json -InputObject $launch -Depth 6),$utf8)
    if($registered){Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue}
}
