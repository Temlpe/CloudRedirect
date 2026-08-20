@echo off
REM Assemble the GitHub release files into ..\release-<version>\ and zip them.
REM Run after build_cpp.bat. Version comes from Version.props (ReleaseVersion).
cd /d "%~dp0"

for /f "tokens=3 delims=<>" %%a in ('findstr /c:"<ReleaseVersion>" Version.props') do set "VER=%%a"
if "%VER%"=="" (echo [ERROR] could not read ReleaseVersion from Version.props & exit /b 1)

set "REL=%~dp0..\release-%VER%"
echo [INFO] Packaging version %VER% into %REL% ...
if exist "%REL%" rmdir /s /q "%REL%"
mkdir "%REL%"

copy /y ui\bin\publish\CloudRedirect.exe "%REL%\" >nul || exit /b 1
copy /y build\Release\cloud_redirect.dll "%REL%\" >nul || exit /b 1
copy /y build\Release\cloud_redirect_cli.exe "%REL%\" >nul
copy /y build\Release\workshop_sync_tool.exe "%REL%\" >nul || exit /b 1
copy /y ui\native\steam_api64.dll "%REL%\" >nul || exit /b 1

pushd "%REL%"
for %%f in (CloudRedirect.exe cloud_redirect.dll) do (
  for /f "delims=" %%h in ('certutil -hashfile "%%f" SHA256 ^| findstr /v ":"') do echo %%h> "%%f.sha256"
)
popd

powershell -NoProfile -Command "Compress-Archive -Path '%REL%\*' -DestinationPath '%~dp0..\CloudRedirect-%VER%-release.zip' -Force"
if errorlevel 1 exit /b 1

echo [OK] Release files: %REL%
echo [OK] Zip:           %~dp0..\CloudRedirect-%VER%-release.zip
exit /b 0
