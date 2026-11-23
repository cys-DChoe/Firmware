# 수집기를 로그온 때 자동으로 켜는 작업 스케줄러 등록 (윈도우, 한 번만 실행. 관리자 권한 필요 없음).
# 지우려면:  Unregister-ScheduledTask 'STM bench collector' -Confirm:$false

$name = 'STM bench collector'
$runner = Join-Path $PSScriptRoot 'run_collector.ps1'
$shell = (Get-Command pwsh -ErrorAction SilentlyContinue).Source
if (-not $shell) { $shell = (Get-Command powershell).Source }

$action = New-ScheduledTaskAction -Execute $shell -Argument "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$runner`""
$trigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
# 시간 제한 없음, 배터리여도 돎, 작업 자체가 죽으면 1분 뒤 다시 (999번까지), 이미 돌고 있으면 새로 안 띄움
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew -StartWhenAvailable
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Settings $settings -Principal $principal `
    -Description 'STM32 센서 수집기 (collector.py) 를 로그온 때 켜고, 죽으면 다시 켬. D:\STM\migration\server\run_collector.ps1' -Force | Out-Null
Get-ScheduledTask -TaskName $name | Select-Object TaskName, State
