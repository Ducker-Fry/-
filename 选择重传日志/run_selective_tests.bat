@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

:: ========== 选择性重传可执行文件 ==========
set EXE=datalink_selective.exe
:: ===========================================

:: 工作目录（脚本所在目录）
cd /d "%~dp0"

echo 即将启动 5 组选择性重传测试，每组运行 20 分钟。
echo 日志文件会分别保存为 test1_sr_A.log, test1_sr_B.log ... test5_sr_A.log, test5_sr_B.log
echo.

:: ---------- 启动所有 B 端（必须先监听） ----------
echo [1/10] 启动测试1 B端...
start "Test1_SR_B" cmd /k "%EXE% -u -t 1200 -p 59144 -l test1_sr_B.log B"
echo [2/10] 启动测试2 B端...
start "Test2_SR_B" cmd /k "%EXE% -i -t 1200 -p 59145 -l test2_sr_B.log B"
echo [3/10] 启动测试3 B端...
start "Test3_SR_B" cmd /k "%EXE% -u -f -t 1200 -p 59146 -l test3_sr_B.log B"
echo [4/10] 启动测试4 B端...
start "Test4_SR_B" cmd /k "%EXE% -f -t 1200 -p 59147 -l test4_sr_B.log B"
echo [5/10] 启动测试5 B端...
start "Test5_SR_B" cmd /k "%EXE% -f -b 1e-4 -t 1200 -p 59148 -l test5_sr_B.log B"

:: 等待 B 端全部进入监听状态
echo 等待 5 秒，确保所有 B 端准备就绪...
timeout /t 5 /nobreak >nul

:: ---------- 启动所有 A 端 ----------
echo [6/10] 启动测试1 A端...
start "Test1_SR_A" cmd /k "%EXE% -u -t 1200 -p 59144 -l test1_sr_A.log A"
echo [7/10] 启动测试2 A端...
start "Test2_SR_A" cmd /k "%EXE% -t 1200 -p 59145 -l test2_sr_A.log A"
echo [8/10] 启动测试3 A端...
start "Test3_SR_A" cmd /k "%EXE% -u -f -t 1200 -p 59146 -l test3_sr_A.log A"
echo [9/10] 启动测试4 A端...
start "Test4_SR_A" cmd /k "%EXE% -f -t 1200 -p 59147 -l test4_sr_A.log A"
echo [10/10] 启动测试5 A端...
start "Test5_SR_A" cmd /k "%EXE% -f -b 1e-4 -t 1200 -p 59148 -l test5_sr_A.log A"

echo.
echo 全部 10 个进程已启动！测试将自动运行 20 分钟后结束。
echo 可以关闭本窗口，不影响后台测试。
echo.
pause