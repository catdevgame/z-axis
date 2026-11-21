# Builds dist\Dave Mirra Freestyle BMX 2\ on Windows x64: the release runner
# with the app front end for DM2 (SLUS_201.59), the game's files from your
# disc (disc\). Derived from the Aggressive Inline repo's
# tools/make-windows.ps1 (no Xbox files, no icon, FFmpeg given prebuilt or
# built by that repo's tools/build-ffmpeg.sh through MSYS2).
#
# Layout (Root = this repo's parent folder on the Windows machine, e.g.
# G:\dave-mirra-bmx2): PS2Recomp\ (branch with the dave_mirra overrides),
# recomp\generated, recomp\generated-vu1, and the disc as -Iso or -DiscDir.
#
#   powershell -ExecutionPolicy Bypass -File tools\make-windows-dm2.ps1 -DiscDir G:\dave-mirra-bmx2\disc -FFmpeg G:\dave-mirra-bmx2\third_party-cache\ffmpeg-...
#   ... -SkipBuild    # package the existing release build
#
# Needs: Visual Studio 2022 (Build Tools) with the C++ workload, Clang/LLVM
# (clang-cl), C++ CMake tools and the Windows SDK. For the player: settings,
# saves and logs go to %APPDATA%\Dave Mirra Freestyle BMX 2.
param(
    [string]$Iso = "",
    [string]$DiscDir = "",
    [string]$Elf = "",
    [string]$ZaxisTree = "",
    [string]$BuildDir = "",
    [string]$Dist = "",
    [string]$FFmpeg = "",
    [string]$AppVersion = "1.0",
    [switch]$SkipBuild,
    [switch]$NoLto,
    [switch]$Zip
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $ZaxisTree) { $ZaxisTree = Join-Path $Root "PS2Recomp" }
if (-not $BuildDir) { $BuildDir = Join-Path $ZaxisTree "out\build-release-win" }
if (-not $Dist) { $Dist = Join-Path $Root "dist" }
$AppName = "Dave Mirra Freestyle BMX 2"
$AppDir = Join-Path $Dist $AppName

function Say($m) { Write-Host "[make-windows] $m" }
function Die($m) { Write-Host "[make-windows] error: $m" -ForegroundColor Red; exit 1 }
function Sha256($path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower() }

# ---- the ELF the generated code came from ----------------------------------
if (-not $Elf) {
    $cfg = Join-Path $Root "recomp\config.game.toml"
    if (Test-Path $cfg) {
        $m = Select-String -Path $cfg -Pattern '^input\s*=\s*"(.*)"' | Select-Object -First 1
        if ($m -and (Test-Path $m.Matches[0].Groups[1].Value)) { $Elf = $m.Matches[0].Groups[1].Value }
    }
}
if (-not $Elf -and (Test-Path (Join-Path $Root "recomp\disc-root\SLUS_201.59"))) { $Elf = Join-Path $Root "recomp\disc-root\SLUS_201.59" }
if (-not $Elf -or -not (Test-Path $Elf)) { Die "recompiler input ELF not found; pass -Elf <path to SLUS_201.59>" }
$ElfSha = Sha256 $Elf
$ElfName = Split-Path -Leaf $Elf
Say "ELF $ElfName sha256 $ElfSha"

# ---- game files --------------------------------------------------------------
# From the disc image (extracted once per image into dist\.disc-cache with
# Windows' tar, which reads ISO 9660) or an extracted folder; the boot ELF
# must be the one the code was recompiled from.
$DiscCache = Join-Path $Dist ".disc-cache"
if ($DiscDir) {
    if (-not (Test-Path $DiscDir)) { Die "no folder $DiscDir" }
    $GameFiles = (Resolve-Path $DiscDir).Path
} else {
    if (-not $Iso -or -not (Test-Path $Iso)) { Die "pass -Iso <your Dave Mirra Freestyle BMX 2 (USA) .iso> or -DiscDir <extracted disc>" }
    $item = Get-Item $Iso
    $stamp = "$($item.Length) $($item.LastWriteTimeUtc.Ticks) $($item.Name)"
    $stampFile = Join-Path $DiscCache ".stamp"
    if (-not (Test-Path $stampFile) -or (Get-Content -Raw $stampFile).Trim() -ne $stamp) {
        Say "extracting the disc image $($item.Name)"
        Remove-Item -Recurse -Force $DiscCache, "$DiscCache.tmp" -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force "$DiscCache.tmp" | Out-Null
        & tar.exe -xf $Iso -C "$DiscCache.tmp"
        if ($LASTEXITCODE -ne 0) { Die "could not extract $Iso" }
        Move-Item "$DiscCache.tmp" $DiscCache
        Set-Content -NoNewline -Path $stampFile -Value $stamp
    }
    $GameFiles = $DiscCache
}
$cnf = Join-Path $GameFiles "SYSTEM.CNF"
if (-not (Test-Path $cnf)) { Die "no SYSTEM.CNF in $GameFiles" }
$boot = (Get-Content $cnf | Select-String -Pattern 'BOOT2\s*=\s*cdrom0:\\(.*);1' | Select-Object -First 1).Matches[0].Groups[1].Value
if (-not $boot -or -not (Test-Path (Join-Path $GameFiles $boot))) { Die "no boot ELF in $GameFiles (SYSTEM.CNF)" }
$discSha = Sha256 (Join-Path $GameFiles $boot)
if ($boot -ne $ElfName -or $discSha -ne $ElfSha) { Die "the disc boots $boot (sha256 $discSha), not the recompiled $ElfName" }
Say "game files: $boot matches"

# ---- toolchain -------------------------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Die "Visual Studio 2022 (Build Tools) not found" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Die "Visual Studio has no C++ tools" }
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
$env:PATH = "$vs\VC\Tools\Llvm\x64\bin;$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$env:PATH"
foreach ($tool in "clang-cl", "lld-link", "cmake", "ninja") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Die "$tool not found (VS components: Clang/LLVM, C++ CMake tools)" }
}

$Runner = Join-Path $BuildDir "runtime\zx-runner.exe"
if (-not $SkipBuild) {
    # ---- FFmpeg: a prebuilt static MSVC FFmpeg (-FFmpeg, e.g. the Aggressive
    # Inline repo's third_party-cache\ffmpeg-*), as tools/build-ffmpeg.sh makes it.
    if (-not $FFmpeg -or -not (Test-Path (Join-Path $FFmpeg "lib"))) { Die "pass -FFmpeg <prefix with include\ and lib\ of a static FFmpeg>" }
    $FFmpeg = (Resolve-Path $FFmpeg).Path -replace '\\','/'
    Say "FFmpeg: $FFmpeg"

    # ---- release runner -------------------------------------------------------
    $vu1 = Join-Path $Root "recomp\generated-vu1"
    $vu1Arg = "-DZX_VU1_AOT_DIR="
    if (Get-ChildItem -Path $vu1 -Filter "vu1_aot_*.cpp" -ErrorAction SilentlyContinue) { $vu1Arg = "-DZX_VU1_AOT_DIR=$($vu1 -replace '\\','/')" }
    $lto = if ($NoLto) { "OFF" } else { "ON" }
    $cmakeArgs = @("-S", $ZaxisTree, "-B", $BuildDir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl", "-DCMAKE_LINKER=lld-link",
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", "-DZX_ENABLE_LTO=$lto",
        "-DZX_RUNNER_GENERATED_DIR=$((Join-Path $Root 'recomp\generated') -replace '\\','/')", $vu1Arg,
        "-DZX_BUILD_RECOMPILER=OFF", "-DZX_BUILD_TEST=OFF",
        "-DZX_ENABLE_RUNTIME_LOGS=OFF", "-DZX_ENABLE_AGRESSIVE_LOGS=OFF", "-DPS2X_ENABLE_IOP_RPC_TRACE=OFF",
        "-DZX_SHOW_WINDOWS_CONSOLE=OFF", "-DZX_FFMPEG_PREFIX=$FFmpeg",
        "-DZX_APP_NAME=$AppName", "-DZX_APP_GAME_TITLE=Dave Mirra Freestyle BMX 2 (USA)",
        "-DZX_APP_ELF=$ElfName", "-DZX_APP_ELF_SHA256=$ElfSha",
        "-DZX_APP_SAVE_DIR=BASLUS-20159BMX2GAME", "-DZX_APP_VERSION=$AppVersion")
    $cxxFlags = "/DWIN32 /D_WINDOWS /EHsc"
    if ($ProfileData) {
        if (-not (Test-Path $ProfileData)) { Die "no profile $ProfileData" }
        $cxxFlags += " -fprofile-instr-use=$((Resolve-Path $ProfileData).Path -replace '\\','/') -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled"
        Say "PGO profile: $ProfileData"
    }
    $cmakeArgs += "-DCMAKE_CXX_FLAGS=$cxxFlags"
    $cmakeArgs += "-DZX_APP_ICON="
    & cmake @cmakeArgs | Out-Null
    if ($LASTEXITCODE -ne 0) { Die "cmake failed" }
    & ninja -C $BuildDir zx-runner
    if ($LASTEXITCODE -ne 0) { Die "build failed" }
}
if (-not (Test-Path $Runner)) { Die "no runner at $Runner" }

# ---- the folder ----------------------------------------------------------------------
Say "assembling $AppDir"
if (Test-Path $AppDir) { Remove-Item -Recurse -Force $AppDir }
New-Item -ItemType Directory -Force $AppDir | Out-Null
Copy-Item $Runner (Join-Path $AppDir "$AppName.exe")
# Game files: hard links where the volume allows it (no extra space), else copies.
$discOut = Join-Path $AppDir "disc"
$linked = 0
Get-ChildItem -Recurse -File $GameFiles | Where-Object { $_.Name -ne ".stamp" } | ForEach-Object {
    $rel = $_.FullName.Substring($GameFiles.Length).TrimStart('\')
    $dest = Join-Path $discOut $rel
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    try { New-Item -ItemType HardLink -Path $dest -Target $_.FullName -ErrorAction Stop | Out-Null; $linked++ }
    catch { Copy-Item $_.FullName $dest }
}
$lgpl = Get-ChildItem -Path $FFmpeg -Filter "COPYING.LGPLv2.1" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
@"
$AppName $AppVersion for Windows x64: a native port of the PlayStation 2 game,
statically recompiled with PS2Recomp, with the game files of your own disc
(disc\). Do not share this folder: it holds game data.

Settings, controls, saves, mods and logs: %APPDATA%\$AppName
Full screen: Alt+Enter or F11. Settings: settings.ini in that folder.

FFmpeg (LGPL 2.1) is linked statically for the game's movies; its licence is
in FFMPEG-LICENSE.txt. SDL3 is under the zlib licence.
"@ | Set-Content -Path (Join-Path $AppDir "README.txt")
if ($lgpl) { Copy-Item $lgpl.FullName (Join-Path $AppDir "FFMPEG-LICENSE.txt") }
$size = (Get-ChildItem -Recurse -File $AppDir | Measure-Object -Property Length -Sum).Sum / 1MB
Say ("done: $AppDir ({0:N0} MB, {1} game files hard-linked)" -f $size, $linked)
if ($Zip) {
    $zipPath = Join-Path $Dist "$AppName.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath }
    Compress-Archive -Path $AppDir -DestinationPath $zipPath -CompressionLevel Optimal
    Say "zip: $zipPath"
}
exit 0
