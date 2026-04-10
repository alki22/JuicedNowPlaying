@echo off
setlocal

set MSBUILD="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
set PROJECT=%~dp0JuicedNowPlaying.vcxproj
set OUTPUT=%~dp0Release\JuicedNowPlaying.asi
set INSTALL=C:\Games\Juiced\scripts\JuicedNowPlaying.asi

%MSBUILD% "%PROJECT%" /p:Configuration=Release /p:Platform=Win32 /nologo /verbosity:minimal
if errorlevel 1 (
    echo.
    echo BUILD FAILED — not installing.
    exit /b 1
)

copy /Y "%OUTPUT%" "%INSTALL%" >nul
if errorlevel 1 (
    echo.
    echo COPY FAILED — is the game running?
    exit /b 1
)

echo.
echo Installed: %INSTALL%
endlocal
