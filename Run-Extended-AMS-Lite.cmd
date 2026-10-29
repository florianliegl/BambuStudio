@echo off
setlocal
set "amsRoot=%~dp0"
set "amsApp=%amsRoot%build\extended-ams-app\src\Release\bambu-studio.exe"
if exist "%amsRoot%build\extended-ams-connect\bambu-studio.exe" set "amsApp=%amsRoot%build\extended-ams-connect\bambu-studio.exe"
if not exist "%amsApp%" (
    echo The custom Bambu Studio application has not been built yet.
    echo See docs\extended-ams-lite\README.md for build instructions.
    pause
    exit /b 1
)
rem Keep this test application's settings separate from the installed Studio.
for %%I in ("%amsApp%") do set "amsAppDir=%%~dpI"
start "Extended AMS Lite" /D "%amsAppDir%" "%amsApp%" --datadir "%amsRoot%build\extended-ams-userdata" %*
exit /b 0
