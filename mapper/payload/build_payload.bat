@echo off
REM ===========================================================================
REM  build_payload.bat  --  compile kernel_payload.dll
REM
REM  Usage:  build_payload.bat [output_dir]
REM  Default output dir is ..\..\bin
REM
REM  The shellcode is freestanding: no CRT, no imports, no default libs.  Every
REM  kernel API is resolved at runtime from the ntoskrnl base it is handed,
REM  which is why /Zl and an empty link line are correct here.
REM ===========================================================================
setlocal
cd /d %~dp0

set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0..\..\bin
if not exist "%OUT%" mkdir "%OUT%"

set OBJ=%~dp0..\..\build\mapper\payload\kernel_payload.obj
if not exist "%~dp0..\..\build\mapper\payload" mkdir "%~dp0..\..\build\mapper\payload"

echo [1/2] compiling kernel_payload.obj
cl /nologo /c /W3 /GS- /Gs999999 /Gy /Oi- /GR- /EHs-c- /Zl /favor:blend ^
   /DUNICODE ^
   /Fo:"%OBJ%" ^
   kernel_payload.c
if errorlevel 1 goto :err

echo [2/2] linking kernel_payload.dll
link /nologo /DLL /NOENTRY /GS- /OPT:REF /OPT:ICF /DYNAMICBASE:NO ^
   /OUT:"%OUT%\kernel_payload.dll" ^
   "%OBJ%"
if errorlevel 1 goto :err

echo [ok] %OUT%\kernel_payload.dll
exit /b 0

:err
echo [X] kernel_payload build failed
exit /b 1
