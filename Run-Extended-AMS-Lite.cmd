@echo off
setlocal
set "amsRoot=%~dp0"
set "amsApp=%amsRoot%build\extended-ams-app\src\Release\bambu-studio.exe"
set "amsData=%amsRoot%build\extended-ams-userdata"
if exist "%amsRoot%build\extended-ams-connect\bambu-studio.exe" set "amsApp=%amsRoot%build\extended-ams-connect\bambu-studio.exe"
if exist "%amsRoot%build\extended-ams-checked\bambu-studio.exe" (
    set "amsApp=%amsRoot%build\extended-ams-checked\bambu-studio.exe"
    if exist "%amsRoot%build\extended-ams-checked-userdata\BambuStudio.conf" set "amsData=%amsRoot%build\extended-ams-checked-userdata"
)
if exist "%amsRoot%build\extended-ams-execution\BambuStudio.dll" (
    set "amsApp=%amsRoot%build\extended-ams-execution\bambu-studio.exe"
    set "amsData=%amsRoot%build\extended-ams-execution-userdata"
)
if exist "%amsRoot%build\extended-ams-magnets\BambuStudio.dll" (
    set "amsApp=%amsRoot%build\extended-ams-magnets\bambu-studio.exe"
    set "amsData=%amsRoot%build\extended-ams-magnets-userdata"
)
if exist "%amsRoot%build\extended-ams-hotfix\BambuStudio.dll" (
    set "amsApp=%amsRoot%build\extended-ams-hotfix\bambu-studio.exe"
    set "amsData=%amsRoot%build\extended-ams-hotfix-userdata"
)
if exist "%amsRoot%build\extended-ams-colors\BambuStudio.dll" (
    set "amsApp=%amsRoot%build\extended-ams-colors\bambu-studio.exe"
    set "amsData=%amsRoot%build\extended-ams-hotfix-userdata"
)
if exist "%amsRoot%build\extended-ams-overview\BambuStudio.dll" (
    set "amsApp=%amsRoot%build\extended-ams-overview\bambu-studio.exe"
    set "amsData=%amsRoot%build\extended-ams-hotfix-userdata"
)
if not exist "%amsApp%" (
    echo The custom Bambu Studio application has not been built yet.
    echo See docs\extended-ams-lite\README.md for build instructions.
    pause
    exit /b 1
)
rem Keep this test application's settings separate from the installed Studio.
for %%I in ("%amsApp%") do set "amsAppDir=%%~dpI"
start "Extended AMS Lite" /D "%amsAppDir%" "%amsApp%" --datadir "%amsData%" %*
exit /b 0
