# 수집기를 끔 (윈도우). 작업 스케줄러 작업을 멈추고, 남은 파이썬 수집기도 끔.
# 다시 켜려면:  Start-ScheduledTask 'STM bench collector'   (로그온 때는 알아서 켜짐)

Stop-ScheduledTask 'STM bench collector' -ErrorAction SilentlyContinue
$Collector = Join-Path $PSScriptRoot 'collector.py'
Get-CimInstance Win32_Process -Filter "Name like 'python%'" |
    Where-Object { $_.CommandLine -like "*$Collector*" } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
'수집기 끔'
