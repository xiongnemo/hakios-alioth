@echo off
chcp 65001 > nul
setlocal
rem Makes the bootable iOS package for a Redmi K40 / POCO F3 from this kit, in WSL (Windows Subsystem for Linux):
rem assemble.sh does the work (see README.md). The package appears beside this folder, as ios-alioth-k40.
where wsl > nul 2>&1
if errorlevel 1 goto nowsl
wsl -e true > nul 2>&1
if errorlevel 1 goto nodistro
set "KIT=%~dp0"
set "KIT=%KIT:~0,-1%"
for /f "usebackq delims=" %%p in (`wsl wslpath -a "%KIT%"`) do set "WKIT=%%p"
wsl bash "%WKIT%/assemble.sh" %*
echo.
pause
exit /b

:nowsl
echo 需要 WSL：在「管理员」PowerShell 里运行 wsl --install，重启电脑后再运行这个文件。
echo WSL is needed: run "wsl --install" in an administrator PowerShell, restart, then run this file again.
pause
exit /b 1

:nodistro
echo WSL 里还没有 Linux：在 PowerShell 里运行 wsl --install -d Ubuntu，设好用户名和密码后再运行这个文件。
echo WSL has no Linux yet: run "wsl --install -d Ubuntu", set up its user, then run this file again.
pause
exit /b 1
