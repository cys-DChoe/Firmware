# 수집기를 켜 두는 스크립트 (윈도우). 수집기가 죽으면 5초 뒤 다시 켬.
#
# 왜 필요한가: 수집기를 Claude 앱 안에서 띄우면 앱이 꺼질 때 같이 죽음 (2026-11-21 08:41 에 그렇게 2일치 기록이 빔).
# 이 스크립트는 작업 스케줄러 "STM bench collector" 가 로그온 때 띄움 → 앱과 상관없이 돎.
#
#   등록:  이 파일 옆의 register_collector_task.ps1 실행 (한 번만)
#   상태:  Get-ScheduledTask 'STM bench collector' | Get-ScheduledTaskInfo
#   멈춤:  이 파일 옆의 stop_collector.ps1      다시 켬: Start-ScheduledTask 'STM bench collector'
#          (Stop-ScheduledTask 만 하면 이 스크립트만 꺼지고 파이썬 수집기는 남음. 그래서 끄는 스크립트를 따로 둠)
#
# 로그 (덮어쓰지 않고 덧붙임):
#   <기록 폴더>\collector.out.log   수집기가 찍는 글
#   <기록 폴더>\collector.err.log   수집기 오류 (죽은 이유가 여기 남음)
#   <기록 폴더>\collector.run.log   이 스크립트가 켜고 끈 시각 · 종료 코드

$LogDir = 'D:\STM\logs'
$Python = 'D:\STM\h563-mcp\.venv\Scripts\python.exe'
$Collector = Join-Path $PSScriptRoot 'collector.py'

$env:BENCH_LOG_DIR = $LogDir
$env:BENCH_MQTT_CRED = 'D:\STM\mqtt\credentials.txt'
$env:PYTHONIOENCODING = 'utf-8'
# 랜선으로 줄이 안 오면 보드 USB 시리얼에서 대신 읽음 (비우면 안 씀). 포트를 잠깐 비워야 하면 <기록 폴더>\serial.pause 파일을 만듦
$env:BENCH_SERIAL_PORT = 'COM3'

# 이 스크립트는 하나만 돎 (두 번 띄워도 뒤엣것은 바로 끝남)
$mutex = New-Object System.Threading.Mutex($false, 'Global\stm-bench-collector-runner')
if (-not $mutex.WaitOne(0)) { exit 0 }

New-Item -ItemType Directory -Force $LogDir | Out-Null
$runLog = Join-Path $LogDir 'collector.run.log'
function Note([string]$text) { Add-Content -Path $runLog -Value ("{0} {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $text) -Encoding utf8 }

function Stop-LeftoverCollector {
    # 예전에 띄운 수집기가 남아 있으면 끔 (기록 폴더 잠금과 COM 포트를 쥐고 있어서 새 수집기가 못 씀)
    Get-CimInstance Win32_Process -Filter "Name like 'python%'" |
        Where-Object { $_.CommandLine -like "*$Collector*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

while ($true) {
    Stop-LeftoverCollector
    Note "수집기 켬"
    # cmd 로 돌려서 출력을 글자 그대로(UTF-8) 파일 끝에 덧붙임
    $out = Join-Path $LogDir 'collector.out.log'
    $err = Join-Path $LogDir 'collector.err.log'
    $p = Start-Process -FilePath $env:ComSpec -ArgumentList '/d', '/c', "`"`"$Python`" -u `"$Collector`" >> `"$out`" 2>> `"$err`"`"" -WindowStyle Hidden -PassThru -Wait
    Note ("수집기 끝남 (종료 코드 {0}). 5초 뒤 다시 켬" -f $p.ExitCode)
    Start-Sleep -Seconds 5
}
