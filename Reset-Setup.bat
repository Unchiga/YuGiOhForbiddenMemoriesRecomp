@echo off
setlocal EnableExtensions
REM ---------------------------------------------------------------------------
REM Yu-Gi-Oh! Forbidden Memories - Recompiled
REM Reset-Setup.bat: put this install back to "freshly extracted".
REM
REM Removes ONLY what the first-run setup created, so it can be run again:
REM   generated\             the game C generated from your disc
REM   psxrecomp\generated\   the BIOS C
REM   build-release\         the built game
REM   disc\SYSTEM.CNF, disc\SLUS_014.11   (forces the disc to be prepared again)
REM   disc.cfg, bios.cfg, disc_verified.cfg, settings.toml, update_check.txt
REM   and, if you say yes, the downloaded build tools under
REM   %LOCALAPPDATA%\retcomm and %LOCALAPPDATA%\psxrecomp (re-downloaded next run)
REM
REM Never touched: your disc image (.bin/.cue), your saves and save states in
REM Documents\My Games, and the extracted files themselves.
REM
REM Run it from inside the game folder (double-click). Then open the setup exe
REM again and pick your disc.
REM ---------------------------------------------------------------------------

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

echo.
echo This resets the Yu-Gi-Oh! Forbidden Memories setup in:
echo   %ROOT%
echo.
echo It deletes the generated game code, the built game and the remembered
echo disc/BIOS picks, so the first-run setup runs again from scratch.
echo Your disc image and your saves are NOT touched.
echo.
set /p "GO=Continue? [y/N] "
if /I not "%GO%"=="y" goto :cancel

if not exist "%ROOT%\game.toml" (
    echo.
    echo game.toml was not found next to this script.
    echo Put Reset-Setup.bat inside the game folder ^(the one with the setup exe^) and run it there.
    goto :end
)

echo.
call :rmdir "%ROOT%\generated"
call :rmdir "%ROOT%\psxrecomp\generated"
call :rmdir "%ROOT%\build-release"
call :del   "%ROOT%\disc\SYSTEM.CNF"
call :del   "%ROOT%\disc\SLUS_014.11"
call :del   "%ROOT%\disc.cfg"
call :del   "%ROOT%\bios.cfg"
call :del   "%ROOT%\disc_verified.cfg"
call :del   "%ROOT%\settings.toml"
call :del   "%ROOT%\update_check.txt"

echo.
set /p "TC=Also delete the downloaded build tools? They are re-downloaded on the next run. [y/N] "
if /I "%TC%"=="y" (
    call :rmdir "%LOCALAPPDATA%\retcomm\toolchains"
    call :rmdir "%LOCALAPPDATA%\psxrecomp\toolchains"
)

echo.
echo Done. Open the setup exe again and pick your disc image ^(.cue or .bin^).
echo Leave the build window open until the game starts.
goto :end

:rmdir
if exist "%~1\" (
    echo   removing folder %~1
    rmdir /S /Q "%~1"
) else (
    echo   ^(not present^) %~1
)
goto :eof

:del
if exist "%~1" (
    echo   removing file   %~1
    del /F /Q "%~1"
) else (
    echo   ^(not present^) %~1
)
goto :eof

:cancel
echo Nothing was changed.

:end
echo.
pause
endlocal
