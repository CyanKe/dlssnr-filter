@echo off
REM ===========================================================================
REM  register_filter.bat -- register / unregister the DirectShow filter
REM
REM  MUST be run as Administrator: it writes HKCR\CLSID and the DirectShow
REM  filter store (IFilterMapper2).
REM
REM     tools\register_filter.bat              register
REM     tools\register_filter.bat /u           unregister
REM ===========================================================================
setlocal
set DLL=%~dp0..\app\dlssnr_dshow.dll

if /i "%~1"=="/u" goto unregister
if /i "%~1"=="unregister" goto unregister

if not exist "%DLL%" (
  echo [error] not found: %DLL%
  echo         Build it first:  tools\build.bat
  pause
  exit /b 1
)

echo Registering %DLL%
regsvr32 /s "%DLL%"
if errorlevel 1 (
  echo.
  echo FAILED. This script must run from an Administrator command prompt.
  echo Right-click Command Prompt ^> "Run as administrator", then:
  echo     cd /d "%~dp0.."
  echo     tools\register_filter.bat
  exit /b 1
)
echo OK - registered as "DLSS Neural Render (DLSSNR)"
echo.
echo To use it in a player (PotPlayer / MPC-BE / MPC-HC):
echo   1. Add "DLSS Neural Render (DLSSNR)" as an external filter
echo   2. Set it to Preferred / Forced so it is actually inserted
echo   3. Make sure dlssnr_host2.dll and nvngx_dlssnr.dll are in app\
echo.
echo The filter is registered MERIT_DO_NOT_USE, so no player will ever insert
echo it automatically -- that is deliberate.
goto :eof

:unregister
echo Unregistering %DLL%
regsvr32 /s /u "%DLL%"
echo Done.
