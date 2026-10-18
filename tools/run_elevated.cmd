@echo off
REM Run a probe elevated and capture its output.
REM Usage:  run_elevated.cmd <exe> <outfile> [extra args passed to the exe]
setlocal EnableDelayedExpansion

set EXE=%~1
set OUT=%~2
shift
shift
set ARGS=%1 %2 %3 %4 %5 %6 %7 %8
if "%EXE%"=="" (echo usage: run_elevated.cmd ^<exe^> ^<outfile^> [args] & exit /b 1)
if not exist "%~dp0..\out\%EXE%" (
  if exist "%~dp0..\out\%EXE%" (set EXE=%~dp0..\out\%EXE%) else (
    echo [not found: %EXE% & exit /b 1)
)

REM Write a launcher script and elevate THAT.  Passing a long argument string to
REM Start-Process -ArgumentList made cmd receive the command as one literal
REM quoted token, so it printed "chain_test.exe" and exited -- which is what the
REM first version did.  A file avoids the quoting entirely.
set RUN=%TEMP%\elev_run_!RANDOM!.cmd
> "%RUN%" echo @echo off
>>"%RUN%" echo cd /d "%~dp0..\out"
>>"%RUN%" echo "%EXE%" %ARGS% ^> "%OUT%" 2^>^&1
>>"%RUN%" echo exit %%ERRORLEVEL%%

powershell -NoProfile -Command "$p = Start-Process -FilePath '%RUN%' -Verb RunAs -Wait -PassThru -WindowStyle Hidden; exit $p.ExitCode" >nul 2>&1

if exist "%OUT%" (
  type "%OUT%"
) else (
  echo [elevated run produced no output file.
  echo [if a UAC prompt appeared and was cancelled, nothing ran.
)
del "%RUN%" >nul 2>&1
endlocal
