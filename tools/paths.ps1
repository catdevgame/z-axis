# paths.ps1: dot-source it (. tools\paths.ps1) for the z-axis layout in the environment, as paths.sh does for the
# shell: every name in paths.env, unless already set, plus ZAXIS_TOOLING (this folder), ZAXIS (the checkout it is in)
# and, in the workshop's checkout, ZAXIS_WORKSHOP (its workshop\ folder). The workshop's layout
# (workshop\merge\paths.env) first, then paths.env's defaults: the first value of a name wins. $HOME in a value is the
# user's own profile folder, or ZAXIS_HOME: never the environment's HOME or USERPROFILE, so a run with a scratch profile
# still finds the data. Also: Get-ZPython, Invoke-ZGuard TOOL [PATH ...] (paths.sh's zguard) and ZX_AUDIO=off (no sound in test
# runs). The data links are setup.ps1's (setup\setup.ps1).
$_zpHere = $PSScriptRoot
if (-not $env:ZAXIS_TOOLING) { $env:ZAXIS_TOOLING = $_zpHere }
if (-not $env:ZAXIS) { $env:ZAXIS = Split-Path -Parent $_zpHere }
if (-not $env:ZAXIS_MAIN) {
  # (a tree that is no git checkout has none; git's complaint is no error of the caller's, whose
  # $ErrorActionPreference may be Stop)
  $_zpStop = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $_zpCommon = git -C $env:ZAXIS rev-parse --path-format=absolute --git-common-dir 2>$null
  $ErrorActionPreference = $_zpStop
  if ($_zpCommon -and $_zpCommon -match '[\\/]\.git$') { $env:ZAXIS_MAIN = ($_zpCommon -replace '[\\/]\.git$', '') }
}
# (the profile folder Windows keeps for the user, not the environment's USERPROFILE)
$_zpHome = $(if ($env:ZAXIS_HOME) { $env:ZAXIS_HOME } else { [Environment]::GetFolderPath('UserProfile') })
if (-not $env:ZAXIS_WORKSHOP) {
  if (Test-Path (Join-Path $env:ZAXIS 'workshop\merge')) { $env:ZAXIS_WORKSHOP = Join-Path $env:ZAXIS 'workshop' }
  elseif ($env:ZAXIS_MAIN -and (Test-Path (Join-Path $env:ZAXIS_MAIN 'workshop\merge'))) {
    $env:ZAXIS_WORKSHOP = Join-Path $env:ZAXIS_MAIN 'workshop'
  }
}
$_zpFiles = @((Join-Path $(if ($env:ZAXIS_WORKSHOP) { $env:ZAXIS_WORKSHOP } else { $_zpHere }) 'merge\paths.env'),
  (Join-Path $_zpHere 'paths.env'))
foreach ($_zpFile in $_zpFiles) {
  if (-not (Test-Path $_zpFile)) { continue }
  foreach ($_zpLine in Get-Content $_zpFile) {
    if ($_zpLine -match '^\s*(#|$)') { continue }
    $_zpName, $_zpValue = $_zpLine -split '=', 2
    if ([Environment]::GetEnvironmentVariable($_zpName)) { continue }
    # ($NAME and ${NAME}: HOME the user's, else a name set above or in the environment; unknown ones kept as written)
    $_zpValue = [regex]::Replace($_zpValue, '\$\{?(\w+)\}?', {
        param($m)
        $n = $m.Groups[1].Value
        if ($n -eq 'HOME') { return $_zpHome }
        $v = [Environment]::GetEnvironmentVariable($n)
        if ($v) { return $v } else { return $m.Value }
      })
    # (paths.env's values use '/', as the tools join them; Windows takes either)
    [Environment]::SetEnvironmentVariable($_zpName, $_zpValue)
  }
}
if (-not $env:ZX_AUDIO) { $env:ZX_AUDIO = 'off' }

# Get-ZPython: Python 3's program: $env:PYTHON, else python or py on PATH (not the Microsoft Store's stand-in, which
# only offers to install it); $null when there is none.
function Get-ZPython {
  if ($env:PYTHON) { return $env:PYTHON }
  foreach ($c in @(Get-Command python, py -CommandType Application -ErrorAction SilentlyContinue)) {
    if ($c.Source -notlike '*\WindowsApps\*') { return $c.Source }
  }
  return $null
}

# Invoke-ZGuard TOOL [PATH ...]: this script stopped (exit 2, a message saying why) when the working folder or an output
# PATH resolves inside the main checkout, unless the orchestrator's merge and batch scripts run it (ZAXIS_IN_MAIN=1):
# zguard.py says what counts.
function Invoke-ZGuard {
  if ($env:ZAXIS_IN_MAIN -eq '1') { return }
  $guard = Join-Path $env:ZAXIS_TOOLING 'zguard.py'
  if (-not (Test-Path $guard)) { return }
  $python = Get-ZPython
  if (-not $python) { return }
  & $python -I $guard @args
  if ($LASTEXITCODE -ne 0) { exit 2 }
}
Remove-Variable _zpHere, _zpStop, _zpCommon, _zpHome, _zpFiles, _zpFile, _zpLine, _zpName, _zpValue -ErrorAction SilentlyContinue
