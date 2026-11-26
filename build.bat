@echo off
REM DiskIIEM one-click build.
REM Wraps build.ps1 and auto-bypasses the PowerShell execution policy.
REM Usage:  build.bat            (output to build\)
REM         build.bat -BuildDir build2
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %errorlevel%
