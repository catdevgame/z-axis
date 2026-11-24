# run-game.ps1 NAME MAX_SECS [RUNNER]: run-game.sh and run-game-dm2.sh (beside it) on Windows. Runs the game for a
# check, unpaced, in a scratch profile, card and working folder under $env:RUN_DIR (default %TEMP%\ai-runs)\NAME; log
# at <dir>\NAME.log. ZX_* settings pass through (pad script, readable spec, GS_NULL, ...). GAME=dm2 or GAME=bmx in the
# environment runs DM2 or BMX XXX (ELF names the disc copy) as run-game-dm2.sh does; Aggressive Inline otherwise.
#
# ZX_UNPACED=1 is the default: guest time follows EE cycles only, so a run is several times faster than real time and
# the logic is the same. Set ZX_UNPACED=0 for anything timed in host time: fps, frame pacing, audio, visuals.
#
# STOP_TICK=<n>: stop as soon as the game reaches vsync tick n (needs a ZX_STATE_LOG; one is added if missing) instead
# of waiting MAX_SECS. MAX_SECS stays the hard time limit, so no run is ever left behind. The tick is read from the
# state log's file (ZX_STATE_LOG_FILE, one is added if missing) and from the log. A stopped run is waited for: the
# script returns once the runner has exited and its files are closed.
#
# Windows has no SIGTERM: a run stopped here is ended as a kill ends it (a recording of it has no end record).
param([string]$Name, [int]$Secs, [string]$Runner = "")
$ErrorActionPreference = "Stop"
if (-not $Name -or -not $Secs) { "usage: run-game.ps1 <name> <max-secs> [runner]"; exit 2 }
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'paths.ps1')   # the shared environment (no sound: ZX_AUDIO)
$game = $(if ($env:GAME) { $env:GAME } else { 'ai' })
if (-not $Runner) { $Runner = $env:RUNNER }
if (-not $Runner) {
  $Runner = Join-Path $env:ZAXIS $(if ($game -eq 'ai') { 'out\build-ai\runtime\zx-runner.exe' } else { 'out\build-coll\runtime\zx-runner.exe' })
}
# The game's ELF and the word its state log follows (its mode: boot and movies, front end, level).
switch ($game) {
  'ai' {
    $recomp = Join-Path $env:AI_WORKSPACE 'recomp'
    $elf = Join-Path $recomp 'disc-root\SLUS_203.27'; $modeWord = '0x276FE4'
  }
  'dm2' {
    $recomp = Join-Path $env:DM2_WORKSPACE 'recomp'
    $elf = $(if ($env:ELF) { $env:ELF } else { Join-Path $recomp 'disc-root\SLUS_201.59' }); $modeWord = '0x259770'
  }
  'bmx' {
    $elf = $(if ($env:ELF) { $env:ELF } else { Join-Path $env:BMX_DATA 'disc\SLUS_204.15' }); $modeWord = '0x29D040'
  }
  default { "run-game.ps1: GAME is ai, dm2 or bmx, not $game"; exit 2 }
}
$root = $(if ($env:RUN_DIR) { $env:RUN_DIR } else { Join-Path $env:TEMP 'ai-runs' })
$D = Join-Path $root $Name
$log = Join-Path $D "$Name.log"
Invoke-ZGuard run-game.ps1 $D   # (not from or into the main checkout)
foreach ($sub in 'home', 'cwd', 'app', 'shots') { New-Item -ItemType Directory -Force (Join-Path $D $sub) | Out-Null }
# AI starts from the workspace's card (a fresh one when the workspace has none); DM2 and BMX XXX from a fresh one, as
# their routes do.
$card = Join-Path $D 'mc0'
$aiCard = $(if ($game -eq 'ai') { Join-Path $recomp 'run\mc0' } else { '' })
if ($aiCard -and (Test-Path $aiCard) -and -not (Test-Path $card)) { Copy-Item -Recurse $aiCard $card }
New-Item -ItemType Directory -Force $card | Out-Null
if (-not $env:ZX_UNPACED) { $env:ZX_UNPACED = '1' }
$addedStateLog = $false
if ($env:STOP_TICK -and -not $env:ZX_STATE_LOG) { $env:ZX_STATE_LOG = $modeWord; $addedStateLog = $true }
$state = $env:ZX_STATE_LOG_FILE
if ($env:STOP_TICK -and -not $state) {
  $state = Join-Path $D 'stop-tick-state.log'
  Remove-Item -Force $state -ErrorAction SilentlyContinue
  $env:ZX_STATE_LOG_FILE = $state
}
# (the state log writes a line when a word changes; the pad log one at every pad read, so the tick is seen whatever
# the state log's words do: AI's with the state log added here, DM2's and BMX XXX's always, as run-game-dm2.sh)
if ($env:STOP_TICK -and -not $env:ZX_PAD_LOG -and ($addedStateLog -or $game -ne 'ai')) { $env:ZX_PAD_LOG = $modeWord }
# The run's own profile and folders (HOME for the runtime, USERPROFILE and the AppData folders for Windows' own).
$env:HOME = Join-Path $D 'home'; $env:USERPROFILE = $env:HOME
$env:APPDATA = Join-Path $env:HOME 'AppData\Roaming'; $env:LOCALAPPDATA = Join-Path $env:HOME 'AppData\Local'
$env:ZX_APP_SUPPORT_DIR = Join-Path $D 'app'; $env:ZX_APP_LOG_DIR = $env:ZX_APP_SUPPORT_DIR
$env:ZX_SCREENSHOT_DIR = Join-Path $D 'shots'; $env:ZX_DEBUG_UI = '0'; $env:ZX_MC_ROOT = $card
if (-not $env:ZX_GS_BACKEND) { $env:ZX_GS_BACKEND = 'cpu' }
$p = Start-Process -FilePath $Runner -ArgumentList "`"$elf`"" -WorkingDirectory (Join-Path $D 'cwd') -NoNewWindow `
  -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
$null = $p.Handle   # (kept, so the exit code is there after the exit)
$t0 = Get-Date; $why = "time limit"
# The last vsync tick in some text: [state] and [frame:upload] lines of the log (the runner's stdout and stderr, two
# files while it runs), [state] and [pad-state] of the state file ([run:tick] counts something else).
function LastTick([string]$path, [string]$pattern, [int]$tail) {
  if (-not (Test-Path $path)) { return 0 }
  $hit = Get-Content $path -Tail $tail -ErrorAction SilentlyContinue | Select-String -Pattern $pattern |
    Select-Object -Last 1
  if ($hit -and $hit.Line -match 'tick=(\d+)') { return [int64]$Matches[1] }
  return 0
}
while (-not $p.HasExited) {
  if (((Get-Date) - $t0).TotalSeconds -ge $Secs) { break }
  if ($env:STOP_TICK) {
    $tick = (LastTick $log '^\[state\]|^\[frame:upload\]' 400), (LastTick "$log.err" '^\[state\]|^\[frame:upload\]' 400),
      (LastTick $state '^\[state\]|^\[pad-state\]' 2) | Measure-Object -Maximum | ForEach-Object Maximum
    if ($tick -ge [int64]$env:STOP_TICK) { $why = "tick $tick"; break }
  }
  Start-Sleep -Milliseconds 500
}
$elapsed = { [int]((Get-Date) - $t0).TotalSeconds }
if (-not $p.HasExited) {
  Stop-Process -Id $p.Id -Force
  $p.WaitForExit(10000) | Out-Null
  $p.WaitForExit()   # (the runner gone, its dumps and logs closed)
  $result = "stopped at $why after $(& $elapsed) s (unpaced=$env:ZX_UNPACED): $log"
} else {
  $p.WaitForExit()
  $result = "exited rc=$($p.ExitCode) after $(& $elapsed) s: $log"
}
# (stderr after stdout, in one log as the shell's 2>&1)
if (Test-Path "$log.err") { Get-Content "$log.err" | Add-Content $log; Remove-Item -Force "$log.err" }
$result
