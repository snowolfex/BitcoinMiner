@echo off
REM ============================================================
REM  Bitcoin Miner - native Windows build script
REM  Tries MSVC (cl) first, then falls back to MinGW g++.
REM ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo.
echo === Building BitcoinMiner.exe ===
echo.

REM --- Try MSVC via vswhere -------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)

if defined VSPATH (
    if exist "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" (
        echo Using MSVC at: !VSPATH!
        call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
        cl /nologo /EHsc /O2 /std:c++17 /DNDEBUG main.cpp /Fe:BitcoinMiner.exe /link user32.lib gdi32.lib comctl32.lib shell32.lib advapi32.lib ws2_32.lib uxtheme.lib comdlg32.lib d3d11.lib dxgi.lib d3dcompiler.lib pdh.lib gdiplus.lib dwmapi.lib
        if errorlevel 1 goto :mingw
        del /q main.obj 2>nul
        echo.
        echo === BUILD OK: BitcoinMiner.exe ===
        goto :done
    )
)

REM --- Fall back to MinGW g++ ------------------------------------------------
:mingw
where g++ >nul 2>nul
if %errorlevel%==0 (
    echo Using MinGW g++
    g++ -O2 -std=c++17 main.cpp -o BitcoinMiner.exe -lcomctl32 -lgdi32 -luser32 -lshell32 -ladvapi32 -lws2_32 -luxtheme -lcomdlg32 -ld3d11 -ldxgi -ld3dcompiler -lpdh -lgdiplus -ldwmapi -mwindows -municode -static
    if errorlevel 1 exit /b 1
    echo.
    echo === BUILD OK: BitcoinMiner.exe ===
    goto :done
)

echo.
echo ERROR: Build failed or no usable compiler found. Close BitcoinMiner before rebuilding.
exit /b 1

:done
endlocal
