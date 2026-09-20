@echo off
REM ===========================================================================
REM  build.bat -- build dlssnr_dshow.dll
REM
REM  Needs Visual Studio 2022 (or Build Tools) with the C++ workload.
REM  Needs NO NVIDIA SDK: the filter links only the Win32/DirectShow system
REM  libraries and loads the engine host dynamically at runtime.
REM ===========================================================================
setlocal
cd /d "%~dp0.."
set ROOT=%CD%
set OUT=%ROOT%\app
set BUILD=%ROOT%\build
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%BUILD%" mkdir "%BUILD%"

set VCVARS=
for %%P in (
  "E:\MSVC\Product\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) do (
  if exist %%P set VCVARS=%%~P
)
if "%VCVARS%"=="" (
  echo [error] vcvars64.bat not found.
  echo         Install Visual Studio 2022 with the "Desktop development with C++" workload.
  pause
  exit /b 1
)
echo Using %VCVARS%
echo.
echo === building dlssnr_dshow.dll ===
call "%VCVARS%" >nul 2>&1
REM gdi32: the tray icon is drawn at runtime.  comctl32: TRACKBAR.  shell32:
REM Shell_NotifyIcon.  uuid: the IPropertyPage / IPropertyPageSite IIDs.
cl /nologo /EHsc /O2 /utf-8 /W3 /D_WIN32_WINNT=0x0601 ^
   /Fo"%BUILD%\\" "%ROOT%\src\dlssnr_dshow.cpp" /LD /Fe:"%OUT%\dlssnr_dshow.dll" ^
   /link /DEF:"%ROOT%\src\dlssnr_dshow.def" ^
   strmiids.lib ole32.lib oleaut32.lib user32.lib advapi32.lib ^
   gdi32.lib comctl32.lib shell32.lib uuid.lib
if errorlevel 1 ( echo [FAILED] & pause & exit /b 1 )

REM cl leaves an import .lib/.exp next to the DLL; they are not wanted.
del /q "%OUT%\dlssnr_dshow.lib" "%OUT%\dlssnr_dshow.exp" 2>nul

echo.
echo ========================================
echo  Build finished: app\dlssnr_dshow.dll
echo.
echo  Next steps:
echo    1. Put dlssnr_host2.dll and nvngx_dlssnr.dll in app\
echo       (see README -- neither ships with this repository)
echo    2. tools\register_filter.bat   (as Administrator)
echo ========================================
