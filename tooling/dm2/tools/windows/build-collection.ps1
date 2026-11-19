# Builds the Z-Axis Collection (Aggressive Inline + Dave Mirra BMX 2) in this folder.
# The Windows worker's folders: workshop/merge/paths.env's WIN_ROOT (the AI root: its disc copy, MSYS2) and WIN_DM2_ROOT
# (the DM2 checkout, its collection folder and disc), or -Root and -Dm2Root.
param([string]$Root, [string]$Dm2Root)
function WorkshopPath($name) {
    $f = Join-Path $PSScriptRoot "..\..\..\..\workshop\merge\paths.env"
    if (Test-Path $f) { foreach ($l in Get-Content $f) { if ($l -match "^$name=(.*)$") { return $Matches[1] } } }
}
if (-not $Root) { $Root = WorkshopPath "WIN_ROOT" }
if (-not $Dm2Root) { $Dm2Root = WorkshopPath "WIN_DM2_ROOT" }
if (-not $Root -or -not $Dm2Root) { Write-Host "build-collection: no -Root and -Dm2Root (or WIN_ROOT, WIN_DM2_ROOT in the workshop's layout)"; exit 2 }
$args2 = @("-ExecutionPolicy", "Bypass", "-File", "$Dm2Root\collection\tools\make-windows.ps1",
    "-Iso", "`"$Root\iso\Aggressive Inline (USA).iso`"", "-Elf", "$Dm2Root\collection\SLUS_203.27",
    "-Games", "ai,dm2", "-Dm2Root", "$Dm2Root", "-Dm2DiscDir", "$Dm2Root\disc", "-Msys", "$Root\tools\msys64")
$p = Start-Process -NoNewWindow -Wait -PassThru -FilePath powershell -ArgumentList $args2 `
    -RedirectStandardOutput $Dm2Root\collection\build.log -RedirectStandardError $Dm2Root\collection\build.err
"exit $($p.ExitCode)" | Add-Content $Dm2Root\collection\build.log
