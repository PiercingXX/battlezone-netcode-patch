@echo off
REM One-line shim so the Steam launch option stays readable:
REM
REM   cmd /c "%LOCALAPPDATA%\bz-netcode\bz_wrap.bat" %command%
REM
REM Steam's launch-option field does not cope well with a PowerShell invocation
REM full of quoting, so the quoting lives here instead.
REM
REM Everything here is logged to bz_wrap.log as well as the console. Steam runs
REM this in a console that dies with the process, so a launch that failed here
REM used to leave nothing at all behind - no game, no log, no error - which is
REM indistinguishable from "the game crashed instantly" and is exactly what
REM made the Windows launch reports impossible to diagnose.
set "BZLOG=%LOCALAPPDATA%\bz-netcode\bz_wrap.log"
if not exist "%LOCALAPPDATA%\bz-netcode" mkdir "%LOCALAPPDATA%\bz-netcode" 2>nul
>>"%BZLOG%" echo [bat] %DATE% %TIME% invoked with: %*

if not exist "%~dp0bz_wrap.ps1" goto :plain

REM Resolve powershell.exe explicitly: if it is not on PATH (it has been
REM missing from a stripped install), the bare name fails with no output.
set "PSEXE=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PSEXE%" set "PSEXE=powershell"

"%PSEXE%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0bz_wrap.ps1" %*
set "RC=%ERRORLEVEL%"
>>"%BZLOG%" echo [bat] wrapper exited with %RC%
exit /b %RC%

:plain
REM If bz_wrap.ps1 is gone (antivirus quarantine is the usual reason), the one
REM job that still matters is launching the game - do that plain rather than
REM leaving the tester with a Steam button that does nothing.
echo [bz_wrap] bz_wrap.ps1 is missing (quarantined?) - launching the game without the uploader
>>"%BZLOG%" echo [bat] bz_wrap.ps1 MISSING - launching the game without the uploader
%*
set "RC=%ERRORLEVEL%"
>>"%BZLOG%" echo [bat] game exited with %RC% (no uploader)
exit /b %RC%
