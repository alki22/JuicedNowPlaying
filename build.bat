@echo off
setlocal

:: ---------------------------------------------------------------------------
:: Direct cl.exe build — bypasses MSBuild's LTCG incremental-link cache,
:: which silently reuses stale object code after the intermediate directory is
:: wiped.  All 7 translation units are always recompiled from source.
:: ---------------------------------------------------------------------------

set VC_ROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.43.34808
set SDK_ROOT=C:\Program Files (x86)\Windows Kits\10
set SDK_VER=10.0.22621.0

set CL_EXE="%VC_ROOT%\bin\Hostx64\x86\cl.exe"
set LINK_EXE="%VC_ROOT%\bin\Hostx64\x86\link.exe"

set SRC=%~dp0
set OBJ=%~dp0manual_build
set OUTPUT=%OBJ%\JuicedNowPlaying.asi
set INSTALL=C:\Games\Juiced\scripts\JuicedNowPlaying.asi

:: Include paths
set INC=/I"%VC_ROOT%\include" /I"%SDK_ROOT%\Include\%SDK_VER%\um" /I"%SDK_ROOT%\Include\%SDK_VER%\ucrt" /I"%SDK_ROOT%\Include\%SDK_VER%\shared"

:: Lib paths
set LIBPATH=/LIBPATH:"%VC_ROOT%\lib\x86" /LIBPATH:"%SDK_ROOT%\Lib\%SDK_VER%\um\x86" /LIBPATH:"%SDK_ROOT%\Lib\%SDK_VER%\ucrt\x86"

:: Common compiler flags
set CFLAGS=/nologo /c /W3 /O2 /GL /Zi /MD /EHsc /std:c++17 /DWIN32 /DNDEBUG /DJUICEDNOWPLAYING_EXPORTS /D_WINDOWS /D_USRDLL /D_CRT_SECURE_NO_WARNINGS

if not exist "%OBJ%" mkdir "%OBJ%"

echo Compiling...

%CL_EXE% %CFLAGS% %INC% /Yc"pch.h" /Fp"%OBJ%\pch.pch" /Fo"%OBJ%\pch.obj"        "%SRC%pch.cpp"
if errorlevel 1 goto fail

%CL_EXE% %CFLAGS% %INC% /Yu"pch.h" /Fp"%OBJ%\pch.pch" /Fo"%OBJ%\bitmap_font.obj" "%SRC%bitmap_font.cpp"
if errorlevel 1 goto fail

%CL_EXE% %CFLAGS% %INC% /Yu"pch.h" /Fp"%OBJ%\pch.pch" /Fo"%OBJ%\d3d9_hook.obj"   "%SRC%d3d9_hook.cpp"
if errorlevel 1 goto fail

%CL_EXE% %CFLAGS% %INC% /Yu"pch.h" /Fp"%OBJ%\pch.pch" /Fo"%OBJ%\dllmain.obj"     "%SRC%dllmain.cpp"
if errorlevel 1 goto fail

%CL_EXE% %CFLAGS% %INC% /Yu"pch.h" /Fp"%OBJ%\pch.pch" /Fo"%OBJ%\ini_reader.obj"  "%SRC%ini_reader.cpp"
if errorlevel 1 goto fail

%CL_EXE% %CFLAGS% %INC% /Yu"pch.h" /Fp"%OBJ%\pch.pch" /Fo"%OBJ%\music_cfg.obj"   "%SRC%music_cfg.cpp"
if errorlevel 1 goto fail

%CL_EXE% %CFLAGS% %INC% /Yu"pch.h" /Fp"%OBJ%\pch.pch" /Fo"%OBJ%\track_watch.obj" "%SRC%track_watch.cpp"
if errorlevel 1 goto fail

%CL_EXE% %CFLAGS% %INC% /Yu"pch.h" /Fp"%OBJ%\pch.pch" /Fo"%OBJ%\vfd_font.obj"    "%SRC%vfd_font.cpp"
if errorlevel 1 goto fail

echo Linking...
%LINK_EXE% /nologo /DLL /LTCG /DEBUG /SUBSYSTEM:WINDOWS /MACHINE:X86 /OUT:"%OUTPUT%" %LIBPATH% kernel32.lib user32.lib d3d9.lib "%OBJ%\pch.obj" "%OBJ%\bitmap_font.obj" "%OBJ%\d3d9_hook.obj" "%OBJ%\dllmain.obj" "%OBJ%\ini_reader.obj" "%OBJ%\music_cfg.obj" "%OBJ%\track_watch.obj" "%OBJ%\vfd_font.obj"
if errorlevel 1 goto fail

echo Copying to game...
copy /Y "%OUTPUT%" "%INSTALL%" >nul
if errorlevel 1 (
    echo.
    echo COPY FAILED -- is the game running?
    exit /b 1
)

echo.
echo Installed: %INSTALL%
endlocal
exit /b 0

:fail
echo.
echo BUILD FAILED.
endlocal
exit /b 1
