@echo off
REM One-shot build for this machine: VS18 BuildTools via NMake (the normal VS
REM generator does not work here - vswhere cannot see any VS instance).
REM
REM Steps:
REM   1. C++ targets (cloud_redirect.dll, workshop_sync_tool.exe,
REM      cloud760_tool.exe, cloud_redirect_cli.exe) -> build-win/
REM   2. Stage artifacts into build/Release/ (the csproj reads from there)
REM   3. Publish the WPF UI -> ui/bin/publish/CloudRedirect.exe
REM      (close a running CloudRedirect.exe first, it locks the output file)
REM
REM After this, run package.bat to produce the release files.
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b 1
set "CMAKE_EXE=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA_DIR=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "PATH=%NINJA_DIR%;%PATH%"

"%CMAKE_EXE%" -S . -B build-win -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
"%CMAKE_EXE%" --build build-win --target cloud_redirect workshop_sync_tool cloud760_tool cloud_redirect_cli
if errorlevel 1 exit /b 1

echo [INFO] Staging artifacts into build\Release...
mkdir build\Release 2>nul
copy /y build-win\cloud_redirect.dll build\Release\ >nul
copy /y build-win\cloud_redirect_cli.exe build\Release\ >nul
copy /y build-win\workshop_sync_tool.exe build\Release\ >nul

echo [INFO] Publishing CloudRedirect UI...
dotnet publish ui\CloudRedirect.csproj -c Release -r win-x64 --self-contained false -o ui\bin\publish
if errorlevel 1 exit /b 1

echo [OK] Build completed.
exit /b 0
