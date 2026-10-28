[CmdletBinding()]
param([string[]]$Programs=@('socket_dns32'),[int]$Repeats=1,[string[]]$WineOnlyPrograms=@('callback32','callback_worker32','fs_recovery32','unixlib_fault32'),[hashtable]$ProgramArguments=@{},[string]$ReportName='runtime-integration-report',[switch]$StopOnFailure)
$ErrorActionPreference='Stop'
Set-Location "$PSScriptRoot/out"
$env:WINE_NATIVE_BRIDGE="native64.dll"
New-Item -ItemType Directory -Force results|Out-Null
$rows=@()
foreach($program in $Programs) {
    $guestArguments=if($ProgramArguments.ContainsKey($program)){[string]$ProgramArguments[$program]}else{''}
    $expectedExit=if($program -eq 'worker_process_exit32'){37}else{0}
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
            if($timeout){
                # Child processes can inherit these output pipes. End the
                # tree before collecting streams so the timeout stays bounded.
                & "$env:SystemRoot/System32/taskkill.exe" /PID $p.Id /T /F|Out-Null
                if(-not $p.HasExited){$p.Kill()}
                $p.WaitForExit()
            }
            $pattern=switch($program){
                'builtin_process32' { '(?m)^builtin_process32: system command interpreter loads as x86, stays suspended and terminates cleanly passed\r?$' }
                'alignment32' { '(?m)^alignment32: alignment-check retry preserves guest execution without handler delivery passed\r?$' }
                'syscall_breakpoint32' { '(?m)^syscall_breakpoint32: guest watchpoint delivered, syscall watchpoint ignored and later thread creation passed\r?$' }
                'unixlib_fault32' { '(?m)^unixlib_fault32: invalid tables return access-violation status and guest execution continues passed\r?$' }
                'process_create32' { '(?m)^process_create32: child loading, suspended DLL attach, Unicode parameters, inherited handles and redirected output passed\r?$' }
                'named_objects32' { '(?m)^named_objects32: session, Local mappings, events, semaphores and mutexes passed\r?$' }
                'stack_growth32' { 'stack_growth32:.*guard pages.*successfully' }
                'selector_identity32' { '(?m)^selector_identity32: current, duplicated and remote FS descriptors identify guest TEBs passed\r?$' }
                'activation_context32' { '(?m)^activation_context32: verified manifest files, mapping and simple/dependent activation contexts passed\r?$' }
                'worker_process_exit32' { '(?m)^worker_process_exit32: requested process exit 37\r?$' }
                'threadpool_wait32' { '(?m)^threadpool_wait32: pending/completed callback wait, work close and pool lifetime passed\r?$' }
                'teb32' { '(?m)^teb32: FS/TEB survived 1000 Windows call and scheduling cycles\r?$' }
                'condition_wake32' { '(?m)^condition_wake32: eight waiters wake and thread lifetimes finish passed\r?$' }
                'thread_delayed_context32' { '(?m)^thread_delayed_context32: normal and forced exit retain valid context-handle errors after cleanup passed\r?$' }
                'fs_recovery32' { 'fs_recovery32:.*recovered and retried' }
                'stack_overflow32' { 'stack_overflow32:.*repeated growth passed' }
                default { "$program`:.*passed" }
            }
            $modeMatches=$true
            if($program -eq 'process_create32') {
                $modeMatches=$out.Result -match '(?m)^phase=unsuspended-child\r?$' -and
                             $out.Result -match '(?m)^phase=suspended-child\r?$' -and
                             $out.Result -match '(?m)^completed.children=00000002\r?$'
            }
            if($program -eq 'io_thread_exit32' -and $guestArguments -match '(^|\s)--spin($|\s)') {
                $modeMatches=$out.Result -match 'spin-issuer mode:\s*00000001'
            }
            if($program -eq 'worker_process_exit32') {
                $modeMatches=$out.Result -match '(?m)^process_exit_library32: process attach\r?$' -and
                             $out.Result -match '(?m)^process_exit_library32: process detach\r?$' -and
                             $out.Result -notmatch 'unexpected.main.survived-process-exit'
                if($guestArguments -match '(^|\s)--main($|\s)'){$modeMatches=$modeMatches -and $out.Result -match '(?m)^mode=main\r?$'}
                elseif($guestArguments -match '(^|\s)--spin($|\s)'){$modeMatches=$modeMatches -and $out.Result -match '(?m)^mode=worker-spin\r?$' -and $out.Result -match 'phase=spinning-main'}
                else{$modeMatches=$modeMatches -and $out.Result -match '(?m)^mode=worker\r?$'}
                if(-not $native -and $guestArguments -notmatch '(^|\s)--main($|\s)') {
                    $modeMatches=$modeMatches -and $out.Result -match '(?m)^GUEST_EXIT code=37 process_shutdown=1 HOST_RETURNED\r?$'
                }
            }
            if($program -eq 'threadpool_wait32') {
                $expectedMode=if($guestArguments -match '(^|\s)--finished($|\s)'){'finished'}else{'pending'}
                $modeMatches=$out.Result -match "(?m)^mode=$expectedMode\r?$" -and
                             $out.Result -match '(?m)^phase=before-work-wait\r?$' -and
                             $out.Result -match '(?m)^phase=after-work-wait\r?$' -and
                             $out.Result -notmatch 'watchdog.phase='
            }
            if($program -eq 'condition_wake32') {
                $expectedMode=if($guestArguments -match '(^|\s)--single($|\s)'){'single'}else{'all'}
                $modeMatches=$out.Result -match "(?m)^mode=$expectedMode\r?$" -and
                             $out.Result -match '(?m)^rounds=00000008\r?$' -and
                             $out.Result -match '(?m)^completed.waiters=00000040\r?$'
            }
            $loaderMatches=$native -or ($out.Result -match 'WINE_LOAD_APPLICATION status=00000000' -and
                                       $out.Result -match "GUEST_EXIT code=$expectedExit .*HOST_RETURNED")
            $rows+=[pscustomobject]@{probe=$program;native=$native;iteration=$i;pid=$p.Id;program=$p.StartInfo.FileName;arguments=$p.StartInfo.Arguments;workingDirectory=$p.StartInfo.WorkingDirectory;exitCode=$p.ExitCode;expectedExit=$expectedExit;timedOut=$timeout;timeoutSeconds=15;markerPattern=$pattern;modeMatches=$modeMatches;loaderMatches=$loaderMatches;passed=(-not $timeout -and $p.ExitCode -eq $expectedExit -and $out.Result -match $pattern -and $modeMatches -and $loaderMatches);stdout=$out.Result;stderr=$err.Result}
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
