@echo off
REM ===========================================================================
REM  sign_driver.bat  --  self-sign vexdrv.sys so it can load on your own
REM                       machine with Secure Boot OFF and test-signing ON.
REM
REM  Run ONCE from an elevated (Administrator) cmd or PowerShell.
REM
REM  ---------------------------------------------------------------------------
REM  DO NOT COMMIT the .pfx / .cer this script produces.
REM
REM  The PFX contains a PRIVATE KEY. Anyone who has it can sign any kernel
REM  driver as if it were trusted by your machine. It must never be pushed to
REM  a repository. .gitignore already excludes sign\*.pfx and sign\*.cer.
REM  ---------------------------------------------------------------------------
REM
REM  What it does:
REM    1. creates a self-signed kernel-mode certificate
REM    2. imports it into Trusted Root and Trusted Publishers
REM    3. signs vexdrv.sys with it
REM    4. verifies the signature
REM
REM  Usage:
REM    sign_driver.bat                 (prompts for nothing; uses default pass)
REM    set VEXDRV_PFX_PASS=yourpass & sign_driver.bat
REM ===========================================================================
setlocal
cd /d "%~dp0"

REM -------------------------------------------------------------------------
REM  Password for the PFX. Override with the environment variable
REM  VEXDRV_PFX_PASS. There is NO default baked into the script on purpose:
REM  a hardcoded password in a public repo is a credential leak.
REM -------------------------------------------------------------------------
if "%VEXDRV_PFX_PASS%"=="" (
  echo [X] VEXDRV_PFX_PASS is not set.
  echo     Set it before running, e.g.:
  echo         set VEXDRV_PFX_PASS=some-local-password
  echo         sign_driver.bat
  echo.
  echo     This password only protects the local copy of the .pfx. Pick
  echo     anything; it is not a secret you share.
  exit /b 1
)
set PASS=%VEXDRV_PFX_PASS%

REM -------------------------------------------------------------------------
REM  Locate the Windows SDK bin directory. Adjust SDK_BIN if your SDK version
REM  differs -- the folder is named after the installed SDK version.
REM -------------------------------------------------------------------------
if "%SDK_BIN%"=="" set SDK_BIN=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64
set MAKECERT="%SDK_BIN%\MakeCert.exe"
set CERTMGR="%SDK_BIN%\certmgr.exe"
set SIGNTOOL="%SDK_BIN%\signtool.exe"

set PFX=vexdrv.pfx
set CERTNAME=vexdrv-self-signed

echo.
echo === 0/4  locating tools ===
if not exist %MAKECERT% (
  echo [X] MakeCert.exe not found at "%SDK_BIN%"
  echo     Adjust the SDK_BIN variable at the top of this script.
  exit /b 1
)
if not exist %CERTMGR% (
  echo [X] certmgr.exe not found
  exit /b 1
)
if not exist %SIGNTOOL% (
  echo [X] signtool.exe not found
  exit /b 1
)
echo [ok] all tools found

echo.
echo === 1/4  creating self-signed kernel certificate ===
del /q "%PFX%" 2>nul
%MAKECERT% -n "CN=%CERTNAME%" -ss PrivateCertStore -r ^
  -eku 1.3.6.1.4.1.311.61.1.1 ^
  -% -n "%CERTNAME%" -pe -ss My -sr LocalMachine -sky Exchange ^
  -p %PASS% -pe -b 01/01/2025 -e 01/01/2030 >nul 2>&1
if errorlevel 1 (
  echo [!] makecert first form failed, trying simplified form
  %MAKECERT% -n "CN=%CERTNAME%" -ss PrivateCertStore -r ^
    -eku 1.3.6.1.4.1.311.61.1.1 -% -pe -ss My -sr LocalMachine ^
    -p %PASS% -pe -b 01/01/2025 -e 01/01/2030 >nul 2>&1
  if errorlevel 1 (
    echo [X] makecert failed
    exit /b 1
  )
)
echo [ok] certificate created

echo.
echo === 2/4  importing into Trusted Root and Trusted Publishers ===
%CERTMGR% -add -c "Root" -s LocalMachine -n "%CERTNAME%" -t "Root" >nul 2>&1
if errorlevel 1 echo [!] Root import reported an error (may already exist)
%CERTMGR% -add -c "TrustedPublisher" -s LocalMachine -n "%CERTNAME%" -t "TrustedPublisher" >nul 2>&1
if errorlevel 1 echo [!] TrustedPublisher import reported an error
echo [ok] imported

echo.
echo === 3/4  signing vexdrv.sys ===
%SIGNTOOL% sign /v /fd sha256 /f "%PFX%" /p %PASS% vexdrv.sys
if errorlevel 1 (
  echo [X] signing failed
  exit /b 1
)
echo [ok] signed

echo.
echo === 4/4  verifying ===
%SIGNTOOL% verify /v /pa vexdrv.sys
echo.
echo If it says "Successfully Verified", the driver is ready.
echo.
echo Next: copy vexdrv.sys next to bs_esp.exe, then run:
echo   bcdedit /set testsigning on
echo   (reboot)
exit /b 0
