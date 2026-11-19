# dm2irun.ps1: dm2run.ps1 in the logged-on user's interactive session (a
# scheduled task of its own name), waits for it, then removes the task.
param([int]$Secs = 150, [string]$Renderer = "gpu")
$user = (Get-Process explorer -IncludeUserName | Select -First 1).UserName
$action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File G:\dave-mirra-bmx2\scripts\dm2run.ps1 -Secs $Secs -Renderer $Renderer"
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName "dave-mirra-bmx2-test" -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask -TaskName "dave-mirra-bmx2-test"
Start-Sleep 2
while ((Get-ScheduledTask -TaskName "dave-mirra-bmx2-test").State -eq "Running") { Start-Sleep 1 }
Unregister-ScheduledTask -TaskName "dave-mirra-bmx2-test" -Confirm:$false
Get-Content G:\dave-mirra-bmx2\scratch\run\status.txt
