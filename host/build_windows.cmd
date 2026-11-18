@echo off
rem Builds the Windows host (RT64 window, keyboard and controllers) with MSVC.
rem Run from anywhere; RecompiledFuncs\ must already exist (wsl sh recomp/run.sh).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio with the C++ x64 tools was not found & exit /b 1)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set "ROOT=%~dp0.."
rem CONKER_CMAKE_ARGS: more CMake options, e.g. -DCONKER_NETPLAY=ON (netplay, which gets protobuf
rem from Visual Studio's vcpkg: host/src/netplay/net/vcpkg.cmake).
cmake -S "%ROOT%\host" -B "%ROOT%\host\build-win" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo %CONKER_CMAKE_ARGS% || exit /b 1
cmake --build "%ROOT%\host\build-win" %* || exit /b 1
echo Built %ROOT%\host\build-win\ConkerRecomp.exe
