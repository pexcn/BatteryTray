@echo off
setlocal
cd /d "%~dp0"

where cl >nul 2>nul || call :vcvars || exit /b
if not exist build mkdir build

rem CI derives VERSION from the git tag and hands it in (see
rem .github\workflows\build.yml); a local build has no tag context, so 0.0.0.
if not defined VERSION set "VERSION=0.0.0"
rem VERSIONINFO wants four numbers and Explorer shows them as the file version,
rem so the distance stays out of them - 1.0.0.1 would read as a released
rem revision. Only major.minor.patch carry over, padded for a short VERSION.
for /f "tokens=1-3 delims=.-" %%a in ("%VERSION%.0.0") do set "VERSION_QUAD=%%a,%%b,%%c,0"
rem A generated header keeps rc away from /D"VERSION_STR=\"1.2.3\"" quoting games.
(
    echo #define VERSION_QUAD %VERSION_QUAD%
    echo #define VERSION_STR "%VERSION%"
) > build\version.h

rc /nologo /DHAVE_VERSION_H /I src /I build /fo build\BatteryTray.res src\BatteryTray.rc || exit /b

rem UCRT comes from ucrtbase.dll, an inbox system DLL since Windows 10; keeping
rem vcruntime and the STL static leaves the exe a single file with no redist.
cl /nologo /std:c++latest /utf-8 /W4 /permissive- /EHsc /GR- /O2 /GL /Gw /DNDEBUG /MT ^
   /Fobuild\ /Febuild\BatteryTray.exe ^
   src\*.cpp build\BatteryTray.res ^
   /link /LTCG /OPT:REF,ICF /SUBSYSTEM:WINDOWS /RELEASE /NODEFAULTLIB:libucrt.lib ^
   ucrt.lib user32.lib gdi32.lib shell32.lib advapi32.lib cfgmgr32.lib dwmapi.lib || exit /b

echo Built build\BatteryTray.exe %VERSION%
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
