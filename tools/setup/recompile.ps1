# recompile.ps1 [-Ai IMAGE] [-Dm2 IMAGE] [-Bmx IMAGE]: each game's code made from your own disc, before the first app
# build (README "Building"), as recompile.sh does on the Mac:
#   1. its boot ELF taken from the disc image (any game in any format: an ISO, a BIN/CUE dump by its CUE or BIN, a
#      CHD, a CSO or a ZSO, read by the app's own reader, zx-disc) into its workspace's extracted\
#      (games\aggressive\workspace, games\dm2\workspace), and checked to be the USA release the code is for;
#   2. its recompiler config made (recomp\tools\make-config.py) and the recompiler run: recomp\generated (AI),
#      recomp\generated-ns (DM2, the collection's namespaced code).
# ps2_recomp and zx-disc are built first when -Build (default <checkout>\out\build-recomp) has them not, with
# Visual Studio 2022's clang-cl, CMake and Ninja (found with vswhere; no developer prompt needed).
# BMX XXX's is its workspace's one step, games\bmx\workspace\recomp\recompile.py (its ELF, configs, code, VU0 upload and VU1 image).
param(
    [string]$Ai = "",
    [string]$Dm2 = "",
    [string]$Bmx = "",
    [string]$Build = ""
)
# (native tools write warnings to stderr: Windows PowerShell would stop on them under "Stop"; each step checks its
# exit code instead)
$ErrorActionPreference = "Continue"
# (this script is tools\setup\; tools\ is the tools' folder, its parent the checkout)
$Tooling = Split-Path -Parent $PSScriptRoot
$ZAxis = Split-Path -Parent $Tooling
if (-not $Build) { $Build = Join-Path $ZAxis "out\build-recomp" }
function Say($m) { Write-Host "[recompile] $m" }
function Die($m) { Write-Host "[recompile] error: $m" -ForegroundColor Red; exit 1 }
if (-not $Ai -and -not $Dm2 -and -not $Bmx) { Die "pass -Ai <your Aggressive Inline disc image>, -Dm2 <your Dave Mirra Freestyle BMX 2 image> and/or -Bmx <your BMX XXX image>" }

$python = Get-Command python, py -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $python) { Die "Python 3 is needed (python.org)" }
function Py($argList) {
    if ($python.Name -like "py*" -and $python.Name -notlike "python*") { $argList = @("-3") + $argList }
    & $python.Source @argList
}

# Visual Studio's compiler, CMake and Ninja in this process.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Die "Visual Studio 2022 (Build Tools) not found" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Die "Visual Studio has no C++ tools" }
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}

$recomp = Join-Path $Build "recompiler\ps2xRecomp\ps2_recomp.exe"
$disc = Join-Path $Build "iop\zx-disc.exe"
if (-not (Test-Path $recomp) -or -not (Test-Path $disc)) {
    Say "building the recompiler and the disc reader in $Build (the first time fetches their sources)"
    New-Item -ItemType Directory -Force (Split-Path -Parent $Build) | Out-Null
    & cmake -S $ZAxis -B $Build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
        -DCMAKE_LINKER=lld-link -DZX_BUILD_RUNTIME=OFF -DZX_BUILD_TEST=OFF *> "$Build.log"
    if ($LASTEXITCODE -ne 0) { Die "configure failed: $Build.log" }
    & ninja -C $Build ps2_recomp zx-disc *>> "$Build.log"
    if ($LASTEXITCODE -ne 0) { Die "the recompiler did not build: $Build.log" }
}

$games = @(
    @{ Id = "ai"; Image = $Ai; Ws = (Join-Path $ZAxis "games\aggressive\workspace"); Elf = "SLUS_203.27"; Config = "config.game.toml";
       Sha = "1b5e170b7750487b29afd9ac9a5012e41a4e67db9c6c90fcba91511e4c688a1c" },
    @{ Id = "dm2"; Image = $Dm2; Ws = (Join-Path $ZAxis "games\dm2\workspace"); Elf = "SLUS_201.59"; Config = "config.ns.toml";
       Sha = "7c89b999729aeaf36140248be80616cd5738b8eb79cf7bc7c90474c2d7bbdc92" }
)
foreach ($g in $games) {
    if (-not $g.Image) { continue }
    if (-not (Test-Path $g.Image)) { Die "$($g.Id): no disc image $($g.Image)" }
    $extracted = Join-Path $g.Ws "extracted"
    New-Item -ItemType Directory -Force $extracted | Out-Null
    $elfPath = Join-Path $extracted $g.Elf
    Remove-Item -Force $elfPath -ErrorAction SilentlyContinue
    & $disc extract $g.Image $extracted $g.Elf
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $elfPath)) { Die "$($g.Id): no $($g.Elf) on $(Split-Path -Leaf $g.Image): not its PS2 disc?" }
    $sha = (Get-FileHash -Algorithm SHA256 $elfPath).Hash.ToLower()
    if ($sha -ne $g.Sha) { Die "$($g.Id): $(Split-Path -Leaf $g.Image)'s $($g.Elf) is not the USA release's (SHA-256 $sha)" }
    Say "$($g.Id): $($g.Elf) from $(Split-Path -Leaf $g.Image), the USA release"
    Push-Location (Join-Path $g.Ws "recomp")
    try {
        Py @("-I", "tools\make-config.py") | Out-Null
        if ($LASTEXITCODE -ne 0) { Die "$($g.Id): make-config.py failed" }
        $start = Get-Date
        & $recomp $g.Config *> (Join-Path $Build "recompile-$($g.Id).log")
        if ($LASTEXITCODE -ne 0) { Die "$($g.Id): the recompiler failed: $(Join-Path $Build "recompile-$($g.Id).log")" }
        $out = (Select-String -Path $g.Config -Pattern '^output\s*=\s*"(.*)"' | Select-Object -First 1).Matches[0].Groups[1].Value
        $count = (Get-ChildItem -File $out | Where-Object { $_.Name -ne ".ps2recomp-manifest" } | Measure-Object).Count
        Say ("$($g.Id): code in recomp\$out ($count files, {0:N0} s)" -f ((Get-Date) - $start).TotalSeconds)
        # (its VU1 micro memory image, as BMX XXX's recompile.py makes BMX's: the tree's tools\setup\vu1_image.py, into
        # recomp\generated-vu1, which make-windows.ps1 builds the VU1 code ahead of time from)
        $vu1Tool = Join-Path $Tooling "setup\vu1_image.py"
        if (Test-Path $vu1Tool) {
            Py @("-I", $vu1Tool, "--game", $g.Id, "--elf", $elfPath, "--out-dir", "generated-vu1") | ForEach-Object { Say ($_ -replace '^vu1_image: ', '') }
            if ($LASTEXITCODE -ne 0) { Die "$($g.Id): vu1_image.py failed" }
        }
    } finally { Pop-Location }
}
if ($Bmx) {
    if (-not (Test-Path $Bmx)) { Die "bmx: no disc image $Bmx" }
    # (its ELF through zx-disc, as the others', so any format reads, into BMX_RECOMP's extracted\ beside it, where
    # make-windows.ps1 looks for it, as recompile.sh's; recompile.py checks its SHA-256)
    $bmxRecomp = (Py @("-I", "-c", "import sys; sys.path.insert(0, sys.argv[1]); import zpaths; print(zpaths.PATHS['BMX_RECOMP'])", $Tooling) | Select-Object -Last 1)
    if ($LASTEXITCODE -ne 0 -or -not $bmxRecomp) { Die "bmx: no BMX_RECOMP from paths.env (zpaths.py)" }
    $bmxExtracted = Join-Path (Split-Path -Parent $bmxRecomp) "extracted"
    New-Item -ItemType Directory -Force $bmxExtracted | Out-Null
    $bmxElf = Join-Path $bmxExtracted "SLUS_204.15"
    Remove-Item -Force $bmxElf -ErrorAction SilentlyContinue
    & $disc extract $Bmx $bmxExtracted "SLUS_204.15"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $bmxElf)) { Die "bmx: no SLUS_204.15 on $(Split-Path -Leaf $Bmx): not its PS2 disc?" }
    Py @("-I", (Join-Path $ZAxis "games\bmx\workspace\recomp\recompile.py"), "--elf", $bmxElf, "--recompiler", $recomp) | ForEach-Object { Say $_ }
    if ($LASTEXITCODE -ne 0) { Die "bmx: bmx\recomp\recompile.py failed" }
}
