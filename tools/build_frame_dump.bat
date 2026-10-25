@echo off
REM ===========================================================================
REM  build_frame_dump.bat -- build tools\frame_dump.cpp -> build\frame_dump.exe
REM
REM  Same toolchain assumptions as build.bat, plus the NVIDIA NGX SDK in deps\
REM  (the static lib supplies GetCapabilityParameters / DestroyParameters).
REM ===========================================================================
setlocal
cd /d "%~dp0.."
set ROOT=%CD%
set OUT=%ROOT%\build
if not exist "%OUT%" mkdir "%OUT%"

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
  exit /b 1
)
if not exist "%ROOT%\deps\sdk_include\nvsdk_ngx.h" (
  echo [error] deps\sdk_include is missing.
  exit /b 1
)

echo Using %VCVARS%
call "%VCVARS%" >nul 2>&1
if errorlevel 1 ( echo [error] vcvars64.bat failed & exit /b 1 )

echo === building frame_dump.exe ===
cl /nologo /EHsc /O2 /utf-8 /W3 /D_WIN32_WINNT=0x0601 ^
   /Fo"%OUT%\\" /I "%ROOT%\deps\sdk_include" ^
   "%ROOT%\tools\frame_dump.cpp" "%ROOT%\src\dlssnr_engine.cpp" ^
   /Fe:"%OUT%\frame_dump.exe" ^
   /link ole32.lib oleaut32.lib user32.lib advapi32.lib d3d12.lib dxgi.lib ^
   "%ROOT%\deps\sdk_lib\nvsdk_ngx_s.lib"
if errorlevel 1 ( echo [FAILED] & exit /b 1 )
echo.
echo build finished: build\frame_dump.exe
