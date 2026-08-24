@echo off
setlocal
cd /d "%~dp0"

where cl >nul 2>nul || call :vcvars || exit /b
if not exist build mkdir build

rc /nologo /I src /fo build\BatteryTray.res src\BatteryTray.rc || exit /b

cl /nologo /std:c++latest /utf-8 /W4 /permissive- /EHsc /GR- /O2 /GL /DNDEBUG /MT ^
   /Fobuild\ /Febuild\BatteryTray.exe ^
   src\*.cpp build\BatteryTray.res ^
   /link /LTCG /OPT:REF,ICF /SUBSYSTEM:WINDOWS /RELEASE ^
   user32.lib gdi32.lib shell32.lib advapi32.lib || exit /b

echo Built build\BatteryTray.exe
exit /b 0

:vcvars
rem -products * is what makes standalone Build Tools installs visible too.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -find VC\Auxiliary\Build\vcvars64.bat 2^>nul`) do set "VCVARS=%%i"
if not defined VCVARS (
    echo Visual Studio C++ build tools not found.
    exit /b 1
)
call "%VCVARS%" >nul
