@echo off
rem Builds build\BatteryTray.exe: optimized, static CRT, no debug info.
setlocal
cd /d "%~dp0"

where cl >nul 2>nul || call :vcvars || exit /b 1
if not exist build mkdir build || exit /b 1

rc /nologo /I src /fo build\BatteryTray.res src\BatteryTray.rc || exit /b 1

rem /MT drops the VC++ redistributable dependency, /GL + /LTCG + /OPT:REF,ICF
rem remove the unused code, and no /Zi means no PDB and no debug info in the exe.
cl /nologo /std:c++latest /utf-8 /W4 /permissive- /EHsc /GR- /O2 /GL /DNDEBUG /MT ^
   /Fo"build\\" /Fe"build\BatteryTray.exe" src\*.cpp build\BatteryTray.res ^
   /link /LTCG /OPT:REF,ICF /SUBSYSTEM:WINDOWS /RELEASE ^
   user32.lib gdi32.lib shell32.lib advapi32.lib || exit /b 1

echo Built build\BatteryTray.exe
exit /b 0

:vcvars
rem -products * is what makes standalone Build Tools installs visible too.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo Visual Studio C++ build tools not found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b 0
