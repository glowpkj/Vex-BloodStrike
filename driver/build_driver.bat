@echo off
REM ===========================================================================
REM  build_driver.bat  --  compile vexdrv.sys
REM
REM  Usage:  build_driver.bat [output_dir]
REM  Default output dir is ..\build\driver
REM
REM  The image is meant to be manually mapped, so it needs no certificate --
REM  but it DOES need the WDK headers and libs.
REM ===========================================================================

set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0..\build\driver

set OBJDIR=%~dp0..\build\driver
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

REM The shared IOCTL contract has to be on the include path.
set INC=%~dp0..\shared
set WDK_KM=C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\km
set WDK_LIB=C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\km\x64
set CFLAGS=/nologo /c /W3 /GS- /Gs999999 /Gy /Oi- /GR- /EHs-c- /D_KERNEL_MODE /D_AMD64_ /DDDKPROF=0x1000 /I"%INC%" /I"%WDK_KM%"

cd /d "%~dp0"

echo [1/3] compiling vexdrv.obj
cl %CFLAGS% /Fo:"%OBJDIR%\vexdrv.obj" vexdrv.c
if errorlevel 1 goto :err

echo [2/3] compiling protect.obj
cl %CFLAGS% /Fo:"%OBJDIR%\protect.obj" protect.c
if errorlevel 1 goto :err

echo [3/3] linking vexdrv.sys
REM ntoskrnl carries the Io*/Ex*/Mm*/Ps*/Ob* surface; hal/wmip/ndis are pulled
REM in by the DDK headers.  /nodefaultlib keeps the CRT out of a driver image.
link /nologo /driver /subsystem:native /entry:DriverEntry ^
   /nodefaultlib /MERGE:_rdata=_text /opt:ref /LIBPATH:"%WDK_LIB%" ^
   /out:"%OBJDIR%\vexdrv.sys" ^
   "%OBJDIR%\vexdrv.obj" "%OBJDIR%\protect.obj" ^
   ntoskrnl.lib hal.lib wmip.lib ndis.lib
if errorlevel 1 goto :err

if not exist "%OUT%" mkdir "%OUT%"
copy /y "%OBJDIR%\vexdrv.sys" "%OUT%\vexdrv.sys" >nul
if errorlevel 1 goto :err

echo [ok] %OUT%\vexdrv.sys
exit /b 0

:err
echo [X] vexdrv build failed -- is the WDK on PATH and in LIB?
exit /b 1
