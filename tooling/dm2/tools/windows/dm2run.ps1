# dm2run.ps1: the packaged DM2 app in portable mode (data next to the exe)
# with the reference pad script, pictures and a perf log; stopped by its PID.
param([int]$Secs = 150, [string]$Renderer = "gpu")
$d = "G:\dave-mirra-bmx2\dist\Dave Mirra Freestyle BMX 2"
$out = "G:\dave-mirra-bmx2\scratch\run"
New-Item -ItemType Directory -Force $out | Out-Null
"" | Set-Content "$d\portable.txt"
Remove-Item -Recurse -Force "$d\Dave Mirra Freestyle BMX 2 Data" -ErrorAction SilentlyContinue
$env:PS2X_PAD_SCRIPT = "1969v:START:6v,2406v:SYNC=0x259770==1,2564v:DOWN:6v,2596v:DOWN:5v,2678v:CROSS:5v,2967v:CROSS:6v,3250v:CROSS:6v,3533v:CROSS:6v,3849v:CROSS:6v,5466v:SYNC=0x25BCC4==121,5527v:UP:338v,5577v:LEFT:30v,5700v:CROSS:19v,5925v:RIGHT:20v"
$env:PS2X_SHOT_AT = "40:$out\menu.png,100:$out\level.png,120:$out\riding.png"
$env:PS2X_PERF_LOG = "5"
$env:PS2X_GS_BACKEND = $Renderer
$p = Start-Process -FilePath "$d\Dave Mirra Freestyle BMX 2.exe" -WorkingDirectory $d -PassThru -RedirectStandardOutput "$out\stdout.log" -RedirectStandardError "$out\stderr.log"
Start-Sleep $Secs
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; "stopped" | Set-Content "$out\status.txt" } else { ("exited " + $p.ExitCode) | Set-Content "$out\status.txt" }
