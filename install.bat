@echo off
rem Installs NWN:EE controller support into ..\bin\win32 (next to nwmain.exe).
setlocal
set "MOD=%~dp0"
set "GAME=%MOD%..\bin\win32"
set "DLL=%MOD%build\Release\version.dll"

if not exist "%GAME%\nwmain.exe" (
    echo Can't find nwmain.exe in "%GAME%". Keep controller_mod inside the Neverwinter Nights folder.
    exit /b 1
)
if not exist "%DLL%" (
    echo version.dll isn't built yet. Run build.bat first.
    exit /b 1
)
if exist "%GAME%\version.dll" (
    findstr /m /c:"SDL_DYNAPI_entry" "%GAME%\version.dll" >nul || (
        echo "%GAME%\version.dll" belongs to another mod; not overwriting it.
        exit /b 1
    )
)

copy /y "%DLL%" "%GAME%\version.dll" >nul || exit /b 1
copy /y "%MOD%data\gamecontrollerdb.txt" "%GAME%\gamecontrollerdb.txt" >nul || exit /b 1
if exist "%GAME%\nwn_controller.ini" (
    echo Kept your existing nwn_controller.ini.
) else (
    copy /y "%MOD%data\nwn_controller.ini" "%GAME%\nwn_controller.ini" >nul || exit /b 1
)
echo Installed. Start the game normally; see bin\win32\nwn_controller.log if anything misbehaves.
