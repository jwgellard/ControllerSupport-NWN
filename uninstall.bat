@echo off
rem Removes NWN:EE controller support from ..\bin\win32. Your nwn_controller.ini is kept.
setlocal
set "GAME=%~dp0..\bin\win32"

if exist "%GAME%\version.dll" (
    findstr /m /c:"SDL_DYNAPI_entry" "%GAME%\version.dll" >nul && del "%GAME%\version.dll"
)
if exist "%GAME%\gamecontrollerdb.txt" del "%GAME%\gamecontrollerdb.txt"
if exist "%GAME%\nwn_controller.log" del "%GAME%\nwn_controller.log"
echo Uninstalled (nwn_controller.ini left in place).
