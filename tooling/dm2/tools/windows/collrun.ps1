# collrun.ps1 -Game ai|dm2: the Z-Axis Collection in portable mode (data next
# to the exe) for Secs seconds, a picture at the end; stopped by its PID.
param([string]$Game = "dm2", [int]$Secs = 120)
$d = "G:\dave-mirra-bmx2\collection\dist\Z-Axis Collection"
$out = "G:\dave-mirra-bmx2\scratch\coll-$Game"
New-Item -ItemType Directory -Force $out | Out-Null
"" | Set-Content "$d\portable.txt"
if ($Game -eq "dm2") { $env:ZX_PAD_SCRIPT = "1969v:START:6v,2406v:SYNC=0x259770==1,2564v:DOWN:6v,2596v:DOWN:5v,2678v:CROSS:5v,2967v:CROSS:6v,3250v:CROSS:6v,3533v:CROSS:6v,3849v:CROSS:6v,5466v:SYNC=0x25BCC4==121,5527v:UP:338v,5577v:LEFT:30v,5700v:CROSS:19v,5925v:RIGHT:20v"; $env:ZX_SHOT_AT = "110:$out\shot.png" }
else { $env:ZX_PAD_SCRIPT = "7:START,11:START,15:START"; $env:ZX_SHOT_AT = "35:$out\shot.png" }
$env:ZX_PERF_LOG = "5"
$p = Start-Process -FilePath "$d\Z-Axis Collection.exe" -ArgumentList "--game", $Game -WorkingDirectory $d -PassThru
Start-Sleep $Secs
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; "stopped" | Set-Content "$out\status.txt" } else { ("exited " + $p.ExitCode) | Set-Content "$out\status.txt" }
