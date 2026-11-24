# setup.ps1 [-Check]: this checkout made ready on Windows, as setup.sh does on the Mac and Linux. Safe to re-run.
#   1. every PATH in data-links.txt (in a game's workspace\ or tools\) made a link to its folder under AI_DATA or
#      DM2_DATA (paths.env, tools\paths.ps1): the tools find the data where they always did, outside git. A folder is
#      linked as a directory junction (no rights needed); a file as a symbolic link, which Windows allows with
#      Developer Mode on (Settings, System, For developers) or as an administrator, and is said and counted otherwise.
#      A data folder the tools write into (recomp\dumps, logs, run, traces, output; and what building the app makes:
#      extracted\ for the ELF, recomp\generated, generated-ns, generated-vu1, dist) is made if missing; the rest is the
#      workshop's own data (Ghidra projects, the decomp's build, PCSX2 baselines, reference runs), not needed to build
#      or play: counted, listed with $env:VERBOSE=1, left unlinked.
#   2. in the workshop's main checkout, CLAUDE.md a link to workshop\CLAUDE.md (the agents' rules). None in a public
#      checkout.
# -Check: report only, change nothing. Exit 1 if a path is a real file or folder (move it into the data first) or a
# link points elsewhere (-Check).
param([switch]$Check)
$ErrorActionPreference = "Stop"
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'paths.ps1')
$ok = 0; $made = 0; $missing = 0; $bad = 0
$writable = '^(recomp/(dumps|logs|run|traces|output|generated|generated-ns|generated-vu1)|extracted|dist)$'
function Same([string]$a, [string]$b) {
  return [IO.Path]::GetFullPath(($a -replace '/', '\')).TrimEnd('\') -eq [IO.Path]::GetFullPath(($b -replace '/', '\')).TrimEnd('\')
}
# A link's target, or $null for a real file or folder.
function LinkTarget([string]$path) {
  $item = Get-Item -LiteralPath $path -Force
  if (-not $item.LinkType) { return $null }
  return @($item.Target)[0]
}
# A new link: a junction for a folder, a symbolic link for a file. True when made.
function MakeLink([string]$link, [string]$target) {
  New-Item -ItemType Directory -Force (Split-Path -Parent $link) | Out-Null
  $kind = $(if (Test-Path -LiteralPath $target -PathType Container) { 'Junction' } else { 'SymbolicLink' })
  try {
    New-Item -ItemType $kind -Path $link -Target $target | Out-Null
    return $true
  } catch {
    "setup: $link not linked: a file's link needs Developer Mode or an administrator ($($_.Exception.Message))"
    return $false
  }
}
foreach ($line in Get-Content (Join-Path $PSScriptRoot 'data-links.txt')) {
  if ($line -match '^\s*(#|$)') { continue }
  $lpath, $var, $rel = $line -split '\s+', 3
  $data = [Environment]::GetEnvironmentVariable($var)
  if (-not $data) { "setup: ${lpath}: no data folder $var (paths.env)"; $bad++; continue }
  $link = Join-Path $env:ZAXIS ($lpath -replace '/', '\'); $target = Join-Path $data ($rel -replace '/', '\')
  if (Test-Path -LiteralPath $link) {
    $now = LinkTarget $link
    if ($null -eq $now) { "setup: $lpath is a real file or folder: move it to $target first"; $bad++; continue }
    if (Same $now $target) { $ok++; continue }
    if ($Check) { "setup: $lpath -> $now, not $target"; $bad++; continue }
    # (a link alone removed, never what it points to: a junction by [IO.Directory]::Delete without recursion)
    if ((Get-Item -LiteralPath $link -Force) -is [IO.DirectoryInfo]) { [IO.Directory]::Delete($link, $false) }
    else { [IO.File]::Delete($link) }
  }
  if (-not (Test-Path -LiteralPath $target)) {
    if ($rel -match $writable) { if (-not $Check) { New-Item -ItemType Directory -Force $target | Out-Null } }
    else {
      $missing++
      if ($env:VERBOSE) { "setup: no ${target}: $lpath not linked" }
      continue
    }
  }
  if ($Check) { "setup: $lpath not linked"; $bad++; continue }
  if (MakeLink $link $target) { $made++ } else { $bad++ }
}
if ($env:ZAXIS_MAIN -and (Same $env:ZAXIS $env:ZAXIS_MAIN) -and (Test-Path (Join-Path $env:ZAXIS 'workshop\CLAUDE.md'))) {
  $rules = Join-Path $env:ZAXIS 'CLAUDE.md'
  if (Test-Path -LiteralPath $rules) {
    $now = LinkTarget $rules
    if ($null -eq $now -or -not ($now -replace '\\', '/').EndsWith('workshop/CLAUDE.md')) {
      "setup: $rules is not the link to workshop\CLAUDE.md"; $bad++
    }
  } elseif ($Check) { "setup: no $rules"; $bad++ }
  else {
    try { New-Item -ItemType SymbolicLink -Path $rules -Target 'workshop\CLAUDE.md' | Out-Null }
    catch { "setup: $rules not linked: a file's link needs Developer Mode or an administrator"; $bad++ }
  }
}
"setup: data $env:AI_DATA, ${env:DM2_DATA}: $ok links as they were, $made made, $bad wrong; $missing of the workshop's own data folders not there (not needed to build or play the games: `$env:VERBOSE=1 lists them)"
if ($bad -ne 0) { exit 1 }
