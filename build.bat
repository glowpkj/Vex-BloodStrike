@echo off
REM ===========================================================================
REM  build.bat  --  build everything and assemble the output folder
REM
REM  Output: dist\  (next to this script)
REM    bs_esp.exe, vexdrv.sys, kernel_payload.dll
REM    plus whatever BYOVD .sys files you drop in there
REM
REM  Override the output folder with the first argument, or set VEX_OUT.
REM
REM  Prerequisites (one time, see README.md):
REM    * Visual Studio 2022 Build Tools  ("Desktop development with C++")
REM    * Windows Driver Kit
REM    * CMake >= 3.24 on PATH
REM    git is NOT required -- ImGui is vendored in third_party\imgui
REM
REM  Run from an "x64 Native Tools Command Prompt for VS 2022".
REM ===========================================================================
setlocal enabledelayedexpansion

cd /d "%~dp0"
set B=%CD%
if not "%~1"=="" ( set OUT=%~1 ) else ( set OUT=%B%\dist )
set STAGE=

if not exist "%OUT%" mkdir "%OUT%" 2>nul
if not exist "%OUT%" (
  echo [X] cannot create "%OUT%"  -- close anything using it, or edit
  echo     the OUT variable at the top of this script.
  exit /b 1
)

REM ---------------------------------------------------------------- prereqs
echo.
echo === 0/4  checking prerequisites ===

where cmake ^>nul 2^>^&1
if errorlevel 1 (
  echo [X] cmake not on PATH.  Install CMake ^>= 3.24 and reopen the prompt.
  set STAGE=prereq ^& goto :fail
)
for /f "delims=" %%v in ('cmake --version ^| findstr /r "^[0-9]"') do echo [ok] cmake %%v

where cl ^>nul 2^>^&1
if errorlevel 1 (
  echo [X] cl.exe not on PATH.
  echo     Run this from "x64 Native Tools Command Prompt for VS 2022",
  echo     not from PowerShell or cmd.
  set STAGE=prereq ^& goto :fail
)
echo [ok] MSVC toolchain

where link ^>nul 2^>^&1
if errorlevel 1 (
  echo [X] link.exe not on PATH - the MSVC environment is not set up.
  set STAGE=prereq ^& goto :fail
)

if not exist "%ProgramFiles(x86)%\Windows Kits\10\Include" (
  echo [X] Windows SDK headers not found.
  echo     Install the "Windows Driver Kit" from the VS installer,
  echo     Individual Components tab.
  set STAGE=prereq ^& goto :fail
)
echo [ok] Windows SDK / WDK headers

if exist "%B%\third_party\imgui\imgui.h" (
  echo [ok] ImGui vendored in third_party\imgui ^(no git needed^)
) else (
  if not "%IMGUI_DIR%"=="" (
    echo [ok] using IMGUI_DIR=%IMGUI_DIR%
  ) else (
    echo [X] no ImGui found.  Pass one:
    echo       build.bat IMGUI_DIR=C:\path\to\imgui
    set STAGE=prereq ^& goto :fail
  )
)

REM ---------------------------------------------------------------- configure
echo.
echo === 1/4  configure ===
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DVEX_OUTPUT_DIR="%OUT:/=/%" %CMAKE_ARGS%
if errorlevel 1 set STAGE=cmake && goto :fail

REM ---------------------------------------------------------------- bs_esp
echo.
echo === 2/4  bs_esp.exe ===
cmake --build build --config Release --target bs_esp
if errorlevel 1 set STAGE=bs_esp && goto :fail

REM --------------------------------------------------------------- payload
echo.
echo === 3/4  kernel_payload.dll ===
call mapper\payload\build_payload.bat "%OUT%"
if errorlevel 1 set STAGE=payload && goto :fail

REM ---------------------------------------------------------------- driver
echo.
echo === 4/4  vexdrv.sys ===
call driver\build_driver.bat "%OUT%"
if errorlevel 1 set STAGE=driver && goto :fail

REM ---------------------------------------------------------------- assemble
echo.
echo --- assembling "%OUT%" ---
if exist "%B%\build\driver\vexdrv.sys" (
  copy /y "%B%\build\driver\vexdrv.sys" "%OUT%\vexdrv.sys" >nul
  echo [ok] vexdrv.sys
) else (
  echo [!] build\driver\vexdrv.sys not found - the driver build failed or
  echo     was skipped. bs_esp still runs without a kernel driver.
)
if exist "%OUT%\kernel_payload.dll" echo [ok] kernel_payload.dll
if exist "%OUT%\bs_esp.exe"          echo [ok] bs_esp.exe

echo.
echo --- BYOVD ---
set FOUND=
for %%d in (iqvw64e cpuz141 ene rtcore) do (
  if exist "%OUT%\%%d.sys" ( set FOUND=1 & echo [ok] %%d.sys )
)
if "%FOUND%"=="" (
  echo [!] no BYOVD present.
  echo     The ESP still runs without one -- it falls back to the physical
  echo     page-table backend, which is slower but reads the same memory.
  echo     Drop a vulnerable signed driver named iqvw64e.sys ^/^/ cpuz141.sys
  echo     ^/^/ ene.sys ^/^/ rtcore.sys here to enable the kernel mapper.
)

echo.
echo ===========================================================================
echo  BUILD OK  --  run "%OUT%\bs_esp.exe"
echo.
echo  Run it ELEVATED, from the dist folder:
echo    cd "%OUT%"
echo    .\bs_esp.exe --ulw
echo  Full ladder in README.md
echo ===========================================================================
exit /b 0

:fail
echo.
echo ===========================================================================
echo  BUILD FAILED  ^(%STAGE%^)
echo  See README.md "Common failures"
echo ===========================================================================
exit /b 1
