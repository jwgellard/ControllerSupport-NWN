# Starts NWN, skips the intro movies and loads the newest save (the one the Load
# screen selects first). Leaves the game focused, in the module.
$s = Split-Path -Parent $MyInvocation.MyCommand.Path
$dir = 'C:\Program Files (x86)\Steam\steamapps\common\Neverwinter Nights\bin\win32'
Start-Process -FilePath "$dir\nwmain.exe" -WorkingDirectory $dir
Start-Sleep -Seconds 20
& "$s\gamectl.ps1" focus | Out-Null
& "$s\gamectl.ps1" key 1B | Out-Null
Start-Sleep -Seconds 3
& "$s\gamectl.ps1" key 1B | Out-Null
Start-Sleep -Seconds 4
& "$s\gamectl.ps1" click 960 562 | Out-Null   # main menu: Load
Start-Sleep -Seconds 3
& "$s\gamectl.ps1" click 1144 834 | Out-Null  # Load Game: Load
Start-Sleep -Seconds 20
& "$s\gamectl.ps1" focus
Get-Content "$dir\nwn_controller.log" -Tail 4
