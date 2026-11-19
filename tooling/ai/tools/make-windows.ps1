# Builds dist\Aggressive Inline\ on Windows x64: "Aggressive Inline.exe" (the
# release runner with the app front end, no console window, statically
# linked: FFmpeg from tools/build-ffmpeg.sh, SDL3, the C++ runtime), the
# game's files extracted from your disc image (disc\), and an icon made from
# local files. The Windows counterpart of tools/make-app.sh. For your own
# use: the folder holds game data, so do not share it (dist\ is ignored by
# git).
#
#   powershell -ExecutionPolicy Bypass -File tools\make-windows.ps1 -Iso "D:\Aggressive Inline (USA).iso"
#   ... -DiscDir recomp\disc-root     # an already extracted copy of the disc instead of -Iso
#   ... -SkipBuild                    # package the existing release build
#   ... -FromBuild DIR                # package DIR's runner and tools when DIR is this tree's release build
#                                     # with these arguments (its .make-app-stamp, as make-app.sh's: the tree
#                                     # of CMakeLists.txt, runtime and iop as committed, no change to them
#                                     # here, and the configure arguments but the paths of where it was
#                                     # made); else builds as without. Each build writes its stamp (pace1)
#   ... -Zip                          # also write dist\Aggressive Inline.zip
#   ... -Reconfigure                  # run CMake's configure even when the build folder's last one had these
#                                     # arguments (else ninja runs it when one of its inputs changed)
#   ... -ProfileData x.profdata       # profile-guided optimization (clang PGO,
#                                     # about 10% faster; see README "Windows")
#   ... -XboxDir D:\xbox              # the Xbox release's files extracted from your
#                                     # own Xbox disc (root, media or media\assets;
#                                     # default dist\.xbox-cache if it exists): Xbox
#                                     # textures and movies (tools/xbox_pack.py, Python 3)
#   ... -NoXboxMovies                 # Xbox textures only
#   ... -XboxIso "D:\Aggressive Inline (USA).xiso.iso"
#                                     # your Xbox disc image, extracted once into
#                                     # dist\.xbox-cache when -XboxDir is not given
#                                     # (tools/xbox_files.py); the files the app reads
#                                     # as they are (tools/xbox-files.txt) go in xbox\,
#                                     # the Zoo and the Ape made into the PS2's formats
#                                     # (PS2Recomp ps2xXboxConvert) in xbox-converted\
#
# Needs: Visual Studio 2022 (Build Tools) with the C++ workload, its
# Clang/LLVM (clang-cl) and C++ CMake tools components, and the Windows SDK
# (dxc for the GPU renderer's shaders); MSYS2 with make, diffutils and nasm
# for the FFmpeg build (-Msys, default C:\msys64, else tools\msys64 beside
# the z-axis checkout); this workspace's generated code (recomp\generated,
# recomp\generated-vu1: links into the data, as tooling/setup.sh makes them)
# and the z-axis checkout this tooling sits in (-PS2Recomp).
# Settings, saves and logs go to %APPDATA%\Aggressive Inline.
#
#   ... -Games ai,dm2 ... -Dm2XboxIso "D:\Dave Mirra Freestyle BMX 2 (USA)-xbox.iso"
#                                     # DM2's Xbox-only levels Venice and Greenville in
#                                     # games\dm2\xbox-levels (tools\dm2_xbox.py),
#                                     # for its own Free Ride, and its levels' Xbox textures in
#                                     # games\dm2\xbox-pack (ps2xExtras xbox-pack); the app never
#                                     # reads the image.
#                                     # Each game runs on its own riders and levels: none is
#                                     # made into another's.
#
#   ... -GcImage "D:\Aggressive Inline-gc.rvz"
#                                     # your GameCube disc image (RVZ or GCM/ISO): its
#                                     # TUT/Estate (an earlier Tutorial left on that disc)
#                                     # extracted (tools\gc_disc.py, Python 3 with zstd:
#                                     # 3.14's compression.zstd, the zstandard package or
#                                     # the zstd command) and made into the files the game
#                                     # loads (PS2Recomp ps2xGcConvert) in estate\, for Free
#                                     # Skate (Settings > Game > Estate Level); the app never
#                                     # reads the image
#
#   ... -Games ai,dm2 -Dm2Root G:\dave-mirra-bmx2 -Dm2DiscDir G:\dave-mirra-bmx2\disc
#                                     # several games in one app ("Z-Axis Collection",
#                                     # -CollectionName): Dave Mirra Freestyle BMX 2's
#                                     # namespaced code (Dm2Root\recomp\generated-ns,
#                                     # generated-vu1) and its game files; the app asks
#                                     # which game (or --game ai|dm2), each game's files in
#                                     # %APPDATA%\<collection>\games\<id>, an existing
#                                     # Aggressive Inline folder copied in on first start.
#                                     # Default: Aggressive Inline alone, as before.
#
#   ... -NoGameData                   # the app with no game data at all (no disc files, no Xbox or GameCube
#                                     # content, the repository's icon): each game is set up from the
#                                     # player's own disc at first start (Settings > Games, or --setup <disc>).
#                                     # No disc image is read; -Elf (or the config's) is still needed
#   ... -Dm2Iso <image> -BmxIso <image>
#                                     # DM2's and BMX XXX's disc images in place of -Dm2DiscDir and -BmxDiscDir
#                                     # (extracted once into dist\.disc-cache-dm2, -bmx). -Iso, -Dm2Iso and
#                                     # -BmxIso take a disc image in any format (an ISO, a BIN/CUE dump, a CHD,
#                                     # CSO or ZSO: tools\disc_image.py makes the ISO tar.exe reads, Python 3)
#   ... -Games ai,dm2,bmx ... -BmxRoot G:\bmx-xxx -BmxDiscDir G:\bmx-xxx\disc
#                                     # BMX XXX too: its namespaced code
#                                     # (BmxRoot\recomp\generated-ns, generated-vu1 if
#                                     # made) and its game files (SLUS_204.15); the app
#                                     # asks which game (or --game ai|dm2|bmx)
#   ... -BmxXboxIso "D:\BMX XXX-xbox.iso" -BmxGcImage "D:\BMX XXX-gc.rvz"
#                                     # BMX XXX's Xbox disc image and GameCube image (RVZ or
#                                     # GCM), read only: their challenge scripts, fuller French,
#                                     # German and Spanish, level-select previews and Z-Axis logo
#                                     # movie (tools\bmx_releases.py; the movies need ffmpeg and
#                                     # ffprobe on PATH) in games\bmx\releases, each behind its
#                                     # setting, and from the Xbox image its levels' and props' Xbox
#                                     # textures in games\bmx\xbox-pack (ps2xExtras xbox-pack);
#                                     # without either, BMX XXX as its PS2 release alone
param(
    # The set of generated code (the Mac's paths.env GENERATED_SET): each folder with -<set> after its name; empty: the
    # plain folders.
    [string]$GenSet = "",
    [string[]]$Games = @("ai"),
    [string]$CollectionName = "Z-Axis Collection",
    [string]$Dm2Root = "",
    [string]$Dm2DiscDir = "",
    [string]$Dm2XboxIso = "",
    [string]$BmxRoot = "",
    [string]$BmxDiscDir = "",
    [string]$BmxXboxIso = "",
    [string]$BmxGcImage = "",
    [string]$BindDir = "",
    [string]$Iso = "",
    [string]$Dm2Iso = "",
    [string]$BmxIso = "",
    [switch]$NoGameData,
    [string]$DiscDir = "",
    [string]$Elf = "",
    [string]$PS2Recomp = "",
    [string]$BuildDir = "",
    [string]$Dist = "",
    [string]$Msys = "",
    [string]$AppVersion = "1.0",
    [string]$ProfileData = "",
    [string]$XboxDir = "",
    [string]$XboxIso = "",
    [string]$GcImage = "",
    [switch]$NoXboxMovies,
    [switch]$SkipBuild,
    [string]$FromBuild = "",
    [switch]$NoLto,
    [switch]$Reconfigure,
    [switch]$Zip
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
# The z-axis checkout this tooling sits in (<z-axis>\tooling\ai), as tools/make-app.sh.
$ZAxis = Split-Path -Parent (Split-Path -Parent $Root)
if (-not $PS2Recomp) { $PS2Recomp = $ZAxis }
if (-not $BuildDir) { $BuildDir = Join-Path $PS2Recomp "out\build-release-win" }
if (-not $Dist) { $Dist = Join-Path $Root "dist" }
$AppName = "Aggressive Inline"
# (powershell -File passes "ai,dm2" as one string)
$Games = @($Games | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Collection = ($Games.Count -gt 1) -or ($Games[0] -ne "ai")
# DM2's workspace: -Dm2Root, else this tooling's dm2\ (make-app.sh's DM2_ROOT default). The folders the build is
# given are absolute: CMake reads a relative one against runtime\, not the folder it was typed in.
if (-not $Dm2Root -and ($Games -contains "dm2")) { $Dm2Root = Join-Path (Split-Path -Parent $Root) "dm2" }
foreach ($name in "Dm2Root", "BmxRoot", "Dm2DiscDir", "BmxDiscDir", "DiscDir", "BindDir") {
    $value = Get-Variable -Name $name -ValueOnly
    if ($value -and (Test-Path $value)) { Set-Variable -Name $name -Value (Resolve-Path $value).Path }
}
if ($Collection) {
    $AppName = $CollectionName
    if (-not $PSBoundParameters.ContainsKey("BuildDir")) { $BuildDir = Join-Path $PS2Recomp "out\build-release-collection-win" }
}
$AppDir = Join-Path $Dist $AppName

function Say($m) { Write-Host "[make-windows] $m" }
# Took STOPWATCH: its seconds, for the steps' lines (where a run's time goes: winbuild1)
function Took($clock) { "{0:N0} s" -f $clock.Elapsed.TotalSeconds }
function Die($m) { Write-Host "[make-windows] error: $m" -ForegroundColor Red; exit 1 }
function Sha256($path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower() }
# On the rcheck worker's host (the Windows worker), this packaging counts as work there (tools36): worker\run\outside-pack-PID.reg
# (PID and start time, as job.ps1's job files), so the replays size their processes around it (job.ps1 ReplayJobs) and a
# long one leaves it the express cores while it runs (waits.ps1 ShortWorkRunning; tools35's pack took 179 min among the
# replays). Not a .pid: the worker's memguard.py takes no process of it for the worker's. It ends with this process: a
# registration whose process is gone is read as ended, and the next pack removes it.
# (RCHECK_WORKER_RUN, else the workshop's layout: workshop/merge/paths.env's WIN_WORKER_RUN; neither: no registration)
function WorkshopPath($name) {
    $f = Join-Path $PSScriptRoot "..\..\..\workshop\merge\paths.env"
    if (Test-Path $f) { foreach ($l in Get-Content $f) { if ($l -match "^$name=(.*)$") { return $Matches[1] } } }
}
$WorkerRun = $(if ($env:RCHECK_WORKER_RUN) { $env:RCHECK_WORKER_RUN } else { WorkshopPath "WIN_WORKER_RUN" })
if ($WorkerRun -and (Test-Path $WorkerRun)) {
    try {
        foreach ($f in @(Get-ChildItem (Join-Path $WorkerRun "outside-pack-*.reg") -ErrorAction SilentlyContinue)) {
            $x = @((Get-Content $f.FullName -ErrorAction SilentlyContinue | Select-Object -First 1) -split ' ')
            $q = $(if ($x.Count -ge 2) { Get-Process -Id ([int]$x[0]) -ErrorAction SilentlyContinue })
            # (StartTime reads $null for a process that ended after Get-Process: gone too; tools45)
            if (-not ($q -and $null -ne $q.StartTime -and "$($q.StartTime.ToFileTimeUtc())" -eq $x[1])) { Remove-Item -Force $f.FullName -ErrorAction SilentlyContinue }
        }
        $me = Get-Process -Id $PID
        $reg = Join-Path $WorkerRun "outside-pack-$PID.reg"
        "$PID $($me.StartTime.ToFileTimeUtc())" | Set-Content $reg
        Say "registered with the rcheck worker as work there: $reg"
    } catch { Say "not registered with the rcheck worker: $_" }
}

$GS = $(if ($GenSet) { "-$GenSet" } else { "" })

# ---- the ELF the generated code came from ----------------------------------
if (-not $Elf) {
    $cfg = Join-Path $Root "recomp\config.game.toml"
    if (Test-Path $cfg) {
        $m = Select-String -Path $cfg -Pattern '^input\s*=\s*"(.*)"' | Select-Object -First 1
        # (a relative input is the config's folder's, as ps2_recomp reads it)
        $named = if ($m) { $m.Matches[0].Groups[1].Value } else { "" }
        if ($m -and -not [System.IO.Path]::IsPathRooted($named)) { $named = Join-Path (Join-Path $Root "recomp") ($named -replace '/', '\') }
        if ($m -and (Test-Path $named)) { $Elf = (Resolve-Path $named).Path }
        elseif ($m) {
            # (the config names the ELF by the absolute path of the checkout it was made in, a Mac's: the
            # same file is this workspace's extracted\<name>, a link into the data as tooling/setup.sh makes it)
            $local = Join-Path $Root ("extracted\" + (Split-Path -Leaf ($m.Matches[0].Groups[1].Value -replace '/', '\')))
            if (Test-Path $local) { $Elf = $local }
        }
    }
}
if (-not $Elf -and (Test-Path (Join-Path $Root "recomp\disc-root\SLUS_203.27"))) { $Elf = Join-Path $Root "recomp\disc-root\SLUS_203.27" }
if (-not $Elf -or -not (Test-Path $Elf)) { Die "recompiler input ELF not found; pass -Elf <path to SLUS_203.27>" }
$ElfSha = Sha256 $Elf
$ElfName = Split-Path -Leaf $Elf
Say "ELF $ElfName sha256 $ElfSha"

# ExtractDisc IMAGE DEST: the disc's files into DEST with Windows' tar (ISO 9660); any other format (a BIN/CUE
# dump, a CHD, CSO or ZSO, which tar does not read) made a 2048-byte ISO beside DEST first (tools\disc_image.py).
$python = Get-Command python, py -ErrorAction SilentlyContinue | Select-Object -First 1
function PythonArgs($argList) {
    if ($python.Name -like "py*" -and $python.Name -notlike "python*") { return @("-3") + $argList }
    return $argList
}
function ExtractDisc($image, $dest) {
    $from = $image
    if ($python) {
        $rawArgs = PythonArgs @("-I", (Join-Path $Root "tools\disc_image.py"), "is-raw", $image)
        & $python.Source @rawArgs
        if ($LASTEXITCODE -eq 0) {
            $from = "$dest.iso"
            $isoArgs = PythonArgs @("-I", (Join-Path $Root "tools\disc_image.py"), "to-iso", $image, $from)
            & $python.Source @isoArgs | ForEach-Object { Say $_ }
            if ($LASTEXITCODE -ne 0) { return $false }
        }
    }
    & tar.exe -xf $from -C $dest
    $ok = $LASTEXITCODE -eq 0
    if ($from -ne $image) { Remove-Item -Force $from -ErrorAction SilentlyContinue }
    return $ok
}
# ExtractOnce IMAGE CACHE: IMAGE's files in CACHE, extracted again only when the image changed (its stamp).
function ExtractOnce($image, $cache) {
    $item = Get-Item $image
    $stamp = "$($item.Length) $($item.LastWriteTimeUtc.Ticks) $($item.Name)"
    $stampFile = Join-Path $cache ".stamp"
    if (-not (Test-Path $stampFile) -or (Get-Content -Raw $stampFile).Trim() -ne $stamp) {
        Say "extracting the disc image $($item.Name)"
        Remove-Item -Recurse -Force $cache, "$cache.tmp" -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force "$cache.tmp" | Out-Null
        if (-not (ExtractDisc $image "$cache.tmp")) { Die "could not extract $image" }
        Move-Item "$cache.tmp" $cache
        Set-Content -NoNewline -Path $stampFile -Value $stamp
    }
    return $cache
}
if ($NoGameData) { Say "-NoGameData: no game data in the app; each game is set up from the player's disc at first start" }

# ---- game files --------------------------------------------------------------
# From the disc image (extracted once per image into dist\.disc-cache with
# Windows' tar, which reads ISO 9660) or an extracted folder; the boot ELF
# must be the one the code was recompiled from.
$DiscCache = Join-Path $Dist ".disc-cache"
if ($NoGameData) {
    $GameFiles = $null
} elseif ($DiscDir) {
    if (-not (Test-Path $DiscDir)) { Die "no folder $DiscDir" }
    $GameFiles = (Resolve-Path $DiscDir).Path
} else {
    if (-not $Iso -or -not (Test-Path $Iso)) { Die "pass -Iso <your Aggressive Inline (USA) .iso> or -DiscDir <extracted disc>" }
    $item = Get-Item $Iso
    $stamp = "$($item.Length) $($item.LastWriteTimeUtc.Ticks) $($item.Name)"
    $stampFile = Join-Path $DiscCache ".stamp"
    if (-not (Test-Path $stampFile) -or (Get-Content -Raw $stampFile).Trim() -ne $stamp) {
        Say "extracting the disc image $($item.Name)"
        Remove-Item -Recurse -Force $DiscCache, "$DiscCache.tmp" -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force "$DiscCache.tmp" | Out-Null
        if (-not (ExtractDisc $Iso "$DiscCache.tmp")) { Die "could not extract $Iso" }
        Move-Item "$DiscCache.tmp" $DiscCache
        Set-Content -NoNewline -Path $stampFile -Value $stamp
    }
    $GameFiles = $DiscCache
}
if (-not $NoGameData) {
$cnf = Join-Path $GameFiles "SYSTEM.CNF"
if (-not (Test-Path $cnf)) { Die "no SYSTEM.CNF in $GameFiles" }
$boot = (Get-Content $cnf | Select-String -Pattern 'BOOT2\s*=\s*cdrom0:\\(.*);1' | Select-Object -First 1).Matches[0].Groups[1].Value
if (-not $boot -or -not (Test-Path (Join-Path $GameFiles $boot))) { Die "no boot ELF in $GameFiles (SYSTEM.CNF)" }
$discSha = Sha256 (Join-Path $GameFiles $boot)
if ($boot -ne $ElfName -or $discSha -ne $ElfSha) { Die "the disc boots $boot (sha256 $discSha), not the recompiled $ElfName" }
Say "game files: $boot matches"
}

# ---- Xbox textures and movies (optional) ------------------------------------------
# From the Xbox release's files (never read by the app): the Xbox picture for
# each PS2 texture it replaces and the Bink movie for each PS2 movie, made
# into dist\.xbox-pack (rebuilt when the inputs change) and put next to the exe.
$python = Get-Command python, py -ErrorAction SilentlyContinue | Select-Object -First 1
function PythonArgs($argList) {
    if ($python.Name -like "py*" -and $python.Name -notlike "python*") { return @("-3") + $argList }
    return $argList
}
if ($NoGameData) { $XboxIso = ""; $XboxDir = Join-Path $Dist ".no-game-data" }
if (-not $XboxDir -and $XboxIso) {
    # The Xbox release's files from your Xbox disc image, extracted once.
    if (-not $python) { Die "Python 3 is needed to extract the Xbox image" }
    $extractArgs = PythonArgs @("-I", (Join-Path $Root "tools\xbox_files.py"), "extract", "--iso", $XboxIso, "--cache", (Join-Path $Dist ".xbox-cache"))
    & $python.Source @extractArgs | ForEach-Object { Say $_ }
    if ($LASTEXITCODE -ne 0) { Die "could not extract the Xbox image $XboxIso" }
}
if (-not $XboxDir) { $XboxDir = Join-Path $Dist ".xbox-cache" }
$XboxPack = Join-Path $Dist ".xbox-pack"
$XboxFiles = Join-Path $Dist ".xbox-files"
if (Test-Path $XboxDir) {
    if (-not $python) { Die "Python 3 is needed for the Xbox files (or leave out -XboxDir)" }
    $bundleArgs = PythonArgs @("-I", (Join-Path $Root "tools\xbox_files.py"), "bundle", "--cache", $XboxDir, "--out", $XboxFiles)
    & $python.Source @bundleArgs | ForEach-Object { Say $_ }
    if ($LASTEXITCODE -ne 0) { Die "tools/xbox_files.py bundle failed" }
} elseif (Test-Path $XboxFiles) { Remove-Item -Recurse -Force $XboxFiles }
if (Test-Path $XboxDir) {
    $python = Get-Command python, py -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $python) { Die "Python 3 is needed for the Xbox textures (or leave out -XboxDir)" }
    $packArgs = @("-I", (Join-Path $Root "tools\xbox_pack.py"), "--ps2", $GameFiles, "--xbox", $XboxDir, "--out", $XboxPack)
    if ($python.Name -like "py*" -and $python.Name -notlike "python*") { $packArgs = @("-3") + $packArgs }
    if ($NoXboxMovies) { $packArgs += "--no-movies" }
    & $python.Source @packArgs | ForEach-Object { Say $_ }
    if ($LASTEXITCODE -ne 0) { Die "tools/xbox_pack.py failed" }
} else {
    if (Test-Path $XboxPack) { Remove-Item -Recurse -Force $XboxPack }
    Say "no Xbox files ($XboxDir): PS2 textures and movies only"
}

# ---- Dave Mirra Freestyle BMX 2 (-Games with dm2) ---------------------------
if ($Games -contains "dm2") {
    if (-not $Dm2Root -or -not (Test-Path (Join-Path $Dm2Root "recomp\generated-ns$GS\register_functions.cpp"))) { Die "pass -Dm2Root <folder with recomp\generated-ns>" }
    if ($NoGameData) {
        # (the ELF the code was recompiled from, for its SHA-256: the workspace's extracted\ copy)
        $dm2Elf = Join-Path $Dm2Root "extracted\SLUS_201.59"
        if (-not (Test-Path $dm2Elf)) { Die "no $dm2Elf (the DM2 ELF the code was recompiled from)" }
        $Dm2Sha = Sha256 $dm2Elf
    } else {
    if (-not $Dm2DiscDir -and $Dm2Iso) { $Dm2DiscDir = ExtractOnce $Dm2Iso (Join-Path $Dist ".disc-cache-dm2") }
    if (-not $Dm2DiscDir -or -not (Test-Path (Join-Path $Dm2DiscDir "SLUS_201.59"))) { Die "pass -Dm2DiscDir <the extracted DM2 disc> or -Dm2Iso <its image>" }
    $Dm2Sha = Sha256 (Join-Path $Dm2DiscDir "SLUS_201.59")
    }
    Say "DM2 game files: SLUS_201.59 sha256 $Dm2Sha"
}

# ---- BMX XXX (-Games with bmx) -----------------------------------------------
if ($Games -contains "bmx") {
    # (-BmxRoot, else the folder of paths.env's BMX_RECOMP, which tooling\recompile.ps1 -Bmx makes, as make-app.sh's)
    if (-not $BmxRoot) {
        $bmxRecomp = (& $python.Source @(PythonArgs @("-I", "-c", "import sys; sys.path.insert(0, sys.argv[1]); import zpaths; print(zpaths.PATHS['BMX_RECOMP'])", (Split-Path -Parent $Root))) | Select-Object -Last 1)
        if ($bmxRecomp) { $BmxRoot = Split-Path -Parent $bmxRecomp }
    }
    if (-not $BmxRoot -or -not (Test-Path (Join-Path $BmxRoot "recomp\generated-ns$GS\register_functions.cpp"))) { Die "pass -BmxRoot <folder with recomp\generated-ns>" }
    if ($NoGameData) {
        $bmxElf = Join-Path $BmxRoot "disc\SLUS_204.15"
        # (else the one recompile.ps1 -Bmx took from the disc, beside BMX_RECOMP)
        if (-not (Test-Path $bmxElf)) { $bmxElf = Join-Path $BmxRoot "extracted\SLUS_204.15" }
        if (-not (Test-Path $bmxElf)) { Die "no $bmxElf (the BMX XXX ELF the code was recompiled from)" }
        $BmxSha = Sha256 $bmxElf
    } else {
    if (-not $BmxDiscDir -and $BmxIso) { $BmxDiscDir = ExtractOnce $BmxIso (Join-Path $Dist ".disc-cache-bmx") }
    if (-not $BmxDiscDir -or -not (Test-Path (Join-Path $BmxDiscDir "SLUS_204.15"))) { Die "pass -BmxDiscDir <the extracted BMX XXX disc> or -BmxIso <its image>" }
    $BmxSha = Sha256 (Join-Path $BmxDiscDir "SLUS_204.15")
    }
    Say "BMX XXX game files: SLUS_204.15 sha256 $BmxSha"
}

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

$ToolsDir = $BuildDir   # (the runner and the tools: BuildDir's, or FromBuild's when it is this tree's build)
if (-not $SkipBuild) {
    # ---- FFmpeg (static, MSVC, through MSYS2) -----------------------------------
    if (-not $Msys) {
        foreach ($c in "C:\msys64", (Join-Path (Split-Path -Parent $ZAxis) "tools\msys64")) { if (Test-Path "$c\usr\bin\bash.exe") { $Msys = $c; break } }
    }
    if (-not $Msys -or -not (Test-Path "$Msys\usr\bin\bash.exe")) { Die "MSYS2 not found; pass -Msys <msys64 folder> (needs: pacman -S make diffutils nasm)" }
    $ffClock = [System.Diagnostics.Stopwatch]::StartNew()
    $env:MSYSTEM = "MSYS"; $env:CHERE_INVOKING = "1"; $env:MSYS2_PATH_TYPE = "inherit"
    $rootMsys = (& "$Msys\usr\bin\cygpath.exe" -u $Root).Trim()
    $ffMsys = (& "$Msys\usr\bin\bash.exe" -lc "cd '$rootMsys' && tools/build-ffmpeg.sh" | Select-Object -Last 1)
    if ($LASTEXITCODE -ne 0 -or -not $ffMsys) { Die "FFmpeg build failed" }
    $FFmpeg = (& "$Msys\usr\bin\cygpath.exe" -m $ffMsys.Trim()).Trim()
    Say "FFmpeg: $FFmpeg ($(Took $ffClock))"

    # ---- release runner -------------------------------------------------------
    $vu1 = Join-Path $Root "recomp\generated-vu1"
    $vu1Arg = "-DPS2X_VU1_AOT_DIR="
    if ((Get-ChildItem -Path $vu1 -Filter "*.vu1code" -ErrorAction SilentlyContinue) -or
        (Get-ChildItem -Path $vu1 -Filter "vu1_aot_*.cpp" -ErrorAction SilentlyContinue)) { $vu1Arg = "-DPS2X_VU1_AOT_DIR=$($vu1 -replace '\\','/')" }
    $lto = if ($NoLto) { "OFF" } else { "ON" }
    $cmakeArgs = @("-S", $PS2Recomp, "-B", $BuildDir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl", "-DCMAKE_LINKER=lld-link",
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", "-DPS2X_ENABLE_LTO=$lto",
        "-DPS2X_RUNNER_GENERATED_DIR=$((Join-Path $Root "recomp\generated$GS") -replace '\\','/')", $vu1Arg,
        "-DPS2X_BUILD_RECOMPILER=OFF", "-DPS2X_BUILD_TEST=OFF", "-DZAXIS_DEV_CHECKS=OFF",
        "-DPS2X_ENABLE_RUNTIME_LOGS=OFF", "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF", "-DPS2X_ENABLE_IOP_RPC_TRACE=OFF",
        "-DPS2X_SHOW_WINDOWS_CONSOLE=OFF", "-DPS2X_FFMPEG_PREFIX=$FFmpeg",
        "-DPS2X_APP_NAME=$AppName", "-DPS2X_APP_GAME_TITLE=Aggressive Inline (USA)",
        "-DPS2X_APP_ELF=$ElfName", "-DPS2X_APP_ELF_SHA256=$ElfSha",
        "-DPS2X_APP_SAVE_DIR=BASLUS-20327CEAIGAME", "-DPS2X_APP_VERSION=$AppVersion")
    if ($Collection) {
        $appGames = "ai|Aggressive Inline|Aggressive Inline (USA)|$ElfName|$ElfSha|BASLUS-20327CEAIGAME"
        $extraGenerated = @()
        $extraVu1 = @()
        if ($Games -contains "dm2") {
            $appGames += ";dm2|Dave Mirra Freestyle BMX 2|Dave Mirra Freestyle BMX 2 (USA)|SLUS_201.59|$Dm2Sha|BASLUS-20159BMX2GAME/BMX2SAVE.DAT"
            $extraGenerated += (Join-Path $Dm2Root "recomp\generated-ns$GS") -replace '\\','/'
            # (VU1 ahead of time from DM2's micro memory image, or code made from it, when there is one: made by a run
            # of the game, tooling\dm2\BRINGUP.md "VU1 AOT"; else DM2's VU1 runs on the interpreter)
            $dm2Vu1 = Join-Path $Dm2Root 'recomp\generated-vu1'
            if ((Get-ChildItem -Path $dm2Vu1 -Filter '*.vu1code' -ErrorAction SilentlyContinue) -or
                (Get-ChildItem -Path $dm2Vu1 -Filter 'vu1_aot_*.cpp' -ErrorAction SilentlyContinue)) {
                $extraVu1 += $dm2Vu1 -replace '\\','/'
            } else { Say "no DM2 VU1 micro memory image in ${dm2Vu1}: DM2's VU1 runs on the interpreter" }
        }
        if ($Games -contains "bmx") {
            $appGames += ";bmx|BMX XXX|BMX XXX (USA)|SLUS_204.15|$BmxSha|BASLUS-20415BMXXXXGM"
            $extraGenerated += (Join-Path $BmxRoot "recomp\generated-ns$GS") -replace '\\','/'
            # (VU1 ahead of time where the folder has its micro memory image, which the build makes the code from: else
            # BMX's run on the interpreter)
            if (Get-ChildItem -Path (Join-Path $BmxRoot 'recomp\generated-vu1') -Filter '*.vu1code' -ErrorAction SilentlyContinue) {
                $extraVu1 += (Join-Path $BmxRoot 'recomp\generated-vu1') -replace '\\','/'
            }
        }
        if ($extraGenerated.Count) { $cmakeArgs += "-DPS2X_RUNNER_EXTRA_GENERATED_DIRS=$($extraGenerated -join ';')" }
        if ($extraVu1.Count) { $cmakeArgs += "-DPS2X_VU1_AOT_EXTRA_DIRS=$($extraVu1 -join ';')" }
        $cmakeArgs += "-DPS2X_APP_GAMES=$appGames"
        # The other games' bind tables: without them each game's instance of the shared readable engine is not
        # compiled, and DM2 and BMX XXX run the engine as recompiled code. The checkout's own, runtime\tools\bind
        # (committed since tools39: game.cmake builds with those whatever -BindDir says); -BindDir only for a commit from
        # before, the folder with dm2.tsv and bmx.tsv (the Mac's paths.env GAME_BIND_LEGACY).
        $ownBind = Join-Path $PS2Recomp 'runtime\tools\bind'
        if (Test-Path (Join-Path $ownBind 'dm2.tsv')) { $BindDir = $ownBind }
        if (-not $BindDir -or -not (Test-Path $BindDir)) { throw "no bind tables for the collection: a commit from before tools39 needs -BindDir (the folder with dm2.tsv and bmx.tsv)" }
        $cmakeArgs += "-DPS2X_BIND_DIR=$($BindDir -replace '\\','/')"
        Say "collection: $($Games -join ', ') (bind tables $BindDir)"
    }
    $cxxFlags = "/DWIN32 /D_WINDOWS /EHsc"
    if ($ProfileData) {
        if (-not (Test-Path $ProfileData)) { Die "no profile $ProfileData" }
        $cxxFlags += " -fprofile-instr-use=$((Resolve-Path $ProfileData).Path -replace '\\','/') -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled"
        Say "PGO profile: $ProfileData"
    }
    $cmakeArgs += "-DCMAKE_CXX_FLAGS=$cxxFlags"
    # ---- icon (from local files only; nothing new is committed) -------------
    # The local game-art icon when there is one (baseline\, never committed), else the repository's own
    # (runtime\resources\app-icon.png); -NoGameData always the repository's.
    $iconPng = Join-Path $Root "baseline\icon\square.png"
    if ($NoGameData -or -not (Test-Path $iconPng)) { $iconPng = Join-Path $PS2Recomp "runtime\resources\app-icon.png" }
    if (Test-Path $iconPng) {
        New-Item -ItemType Directory -Force $BuildDir | Out-Null
        $ico = Join-Path $BuildDir "app.ico"
        Add-Type -AssemblyName System.Drawing
        $src = [System.Drawing.Image]::FromFile($iconPng)
        $sizes = 16, 24, 32, 48, 64, 128, 256
        $pngs = foreach ($s in $sizes) {
            $bmp = New-Object System.Drawing.Bitmap $s, $s
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.DrawImage($src, 0, 0, $s, $s); $g.Dispose()
            $ms = New-Object System.IO.MemoryStream
            $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
            , $ms.ToArray()
        }
        $src.Dispose()
        # ICO with PNG images (Windows Vista and later).
        $out = New-Object System.IO.MemoryStream
        $w = New-Object System.IO.BinaryWriter $out
        $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
        $offset = 6 + 16 * $sizes.Count
        for ($i = 0; $i -lt $sizes.Count; $i++) {
            $s = $sizes[$i]; $b = if ($s -ge 256) { 0 } else { $s }
            $w.Write([byte]$b); $w.Write([byte]$b); $w.Write([byte]0); $w.Write([byte]0)
            $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
            $offset += $pngs[$i].Length
        }
        foreach ($p in $pngs) { $w.Write($p) }
        # (written only when it changed: a new icon file is a new resource, so the runner is linked again, about
        # 6 minutes of ThinLTO on Windows for a build with nothing else to do: winbuild1)
        $w.Flush(); $icoBytes = $out.ToArray(); $w.Dispose()
        $same = (Test-Path $ico) -and ([System.Convert]::ToBase64String([System.IO.File]::ReadAllBytes($ico)) -eq [System.Convert]::ToBase64String($icoBytes))
        if (-not $same) { [System.IO.File]::WriteAllBytes($ico, $icoBytes) }
        $cmakeArgs += "-DPS2X_APP_ICON=$($ico -replace '\\','/')"
        Say "icon from $iconPng"
    } else {
        Say "no icon ($iconPng); building without one"
        $cmakeArgs += "-DPS2X_APP_ICON="
    }
    # The stamp of a build of this tree with these arguments (make-app.sh's): the build's inputs as committed, none
    # when they have a change here; the arguments but -S, -B, the icon and the fetched sources, every rooted path in a
    # value that exists cut to its last part (the FFmpeg prefix's is its build key); and the data folders they name
    # (the generated code, the VU1 code, the bind tables) by what they are (DataLine). No path is in it: a build made
    # in another checkout packs here when its inputs are the same (pace2).
    # DataLine DIR: a data folder by what it is, as make-app.sh's data_print: "<files> <key>". A folder the recompiler
    # made (its .ps2recomp-manifest: the recompiler's source id, the SHA-256 of the config, the ELF and the Ghidra map,
    # each file it wrote and its size) is that manifest's SHA-256 when its top-level files are exactly the manifest's,
    # with its sizes, and none is newer than the manifest: one directory listing, no file opened (hashing AI's and DM2's
    # 48k generated files took about 52 minutes on Windows under Defender: setup1, winbuild1). Any other folder, or
    # one that differs from its manifest (a file edited by hand, added or left from an older run), is the SHA-256 of its
    # top-level files' names and bytes, as before the manifests. (The files Get-ChildItem lists: not hidden or system
    # ones, nor .DS_Store.)
    function DataLine([string]$dir) {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $skip = [System.IO.FileAttributes]::Hidden -bor [System.IO.FileAttributes]::System
        # (foreach statements, not pipelines: 26k files)
        $kept = New-Object 'System.Collections.Generic.List[System.IO.FileInfo]'
        $manifest = $null
        foreach ($f in ([System.IO.DirectoryInfo]::new($dir)).GetFiles()) {
            if ($f.Name -eq ".DS_Store" -or ($f.Attributes -band $skip)) { continue }
            $kept.Add($f)
            if ($f.Name -ceq ".ps2recomp-manifest") { $manifest = $f }
        }
        $files = $kept.ToArray()
        if ($files.Count -gt 1) { [Array]::Sort([string[]]$files.Name, $files, [System.StringComparer]::Ordinal) }   # (make-app.sh's order)
        if ($manifest) {
            $bytes = [System.IO.File]::ReadAllBytes($manifest.FullName)
            $listed = New-Object 'System.Collections.Generic.Dictionary[string,long]' ([System.StringComparer]::Ordinal)
            $why = ""
            $lines = [System.Text.Encoding]::UTF8.GetString($bytes) -split "`n"
            $at = [Array]::FindIndex($lines, [Predicate[string]]{ param($l) $l.StartsWith("files ") })
            if ($at -lt 0) { $why = "not a manifest" }
            else {
                for ($i = $at + 1; $i -lt $lines.Count; $i++) {
                    if (-not $lines[$i]) { continue }
                    $sp = $lines[$i].IndexOf(' ')
                    $listed[$lines[$i].Substring($sp + 1)] = [long]$lines[$i].Substring(0, $sp)
                }
                $when = $manifest.LastWriteTimeUtc.Ticks
                if ($files.Count - 1 -ne $listed.Count) { $why = "$($files.Count - 1) files, $($listed.Count) in it" }
                foreach ($f in $files) {
                    if ($why) { break }
                    if ($f.Name -ceq ".ps2recomp-manifest") { continue }
                    $size = 0L
                    if (-not $listed.TryGetValue($f.Name, [ref]$size)) { $why = "$($f.Name) not in it" }
                    elseif ($f.Length -ne $size) { $why = "$($f.Name) is $($f.Length) bytes, not $size" }
                    elseif ($f.LastWriteTimeUtc.Ticks -gt $when) { $why = "$($f.Name) is newer than the manifest" }
                }
            }
            if (-not $why) { return "$($files.Count) manifest $(($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString("x2") }) -join '')" }
            Say "$dir is not what its manifest lists ($why): hashing every file"
        }
        $h = [System.Security.Cryptography.IncrementalHash]::CreateHash([System.Security.Cryptography.HashAlgorithmName]::SHA256)
        foreach ($f in $files) {
            $h.AppendData([System.Text.Encoding]::UTF8.GetBytes($f.Name + [char]0))
            $st = [System.IO.File]::OpenRead($f.FullName); try { $h.AppendData($sha.ComputeHash($st)) } finally { $st.Dispose() }
        }
        return "$($files.Count) $(($h.GetHashAndReset() | ForEach-Object { $_.ToString("x2") }) -join '')"
    }
    $stamp = ""
    $stampClock = [System.Diagnostics.Stopwatch]::StartNew()
    $dirty = & git -C $PS2Recomp status --porcelain -- CMakeLists.txt runtime iop 2>$null
    $tree = & git -C $PS2Recomp rev-parse HEAD:CMakeLists.txt HEAD:runtime HEAD:iop 2>$null
    if ($LASTEXITCODE -eq 0 -and -not $dirty -and $tree) {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $hex = { param($t) (($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($t)) | ForEach-Object { $_.ToString("x2") }) -join "").Substring(0, 16) }
        $kept = @(); $data = @(); $skip = $false
        # (an item that can name no path, such as PS2X_APP_GAMES' "ai|Aggressive Inline|...", is no path: .NET
        # Framework's IsPathRooted and Test-Path throw on its characters, which stopped b132's first pack here)
        $isPath = { param($p, $type) $p -and $p.IndexOfAny([System.IO.Path]::GetInvalidPathChars()) -lt 0 -and
                    (Test-Path -LiteralPath $p -PathType $type) }
        foreach ($x in $cmakeArgs) {
            if ($skip) { $skip = $false; continue }
            if ($x -eq "-S" -or $x -eq "-B") { $skip = $true; continue }
            if ($x -like "-DFETCHCONTENT_SOURCE_DIR_*" -or $x -like "-DPS2X_APP_ICON=*") { continue }
            if ($x -match '^(-D[^=]+)=(.*)$') {
                $name = $Matches[1]; $items = $Matches[2] -split ';'
                $kept += "$name=$(($items | ForEach-Object { if ((& $isPath $_ Any) -and [System.IO.Path]::IsPathRooted($_)) { Split-Path -Leaf $_ } else { $_ } }) -join ';')"
                if ($name -ne "-DPS2X_FFMPEG_PREFIX") {
                    for ($n = 0; $n -lt $items.Count; $n++) {
                        if (-not (& $isPath $items[$n] Container)) { continue }
                        $data += "$name $n $(DataLine $items[$n])"
                    }
                }
            } else {
                $kept += $x
            }
        }
        $stamp = "tree $(& $hex (($tree -join "`n") + "`n")) ($(& git -C $PS2Recomp rev-parse --short HEAD))`nargs $(& $hex (($kept -join "`n") + "`n"))`ndata $(& $hex (($data -join "`n") + "`n"))"
        Say ("stamp: {0:N1} s ($($data.Count) data folders)" -f $stampClock.Elapsed.TotalSeconds)
    }
    $key = { param($t) ($t -split "`n" | ForEach-Object { $_ -replace ' \([0-9a-f]+\)$', '' }) -join "`n" }
    $fromStamp = if ($FromBuild -and (Test-Path (Join-Path $FromBuild ".make-app-stamp"))) { (Get-Content -Raw (Join-Path $FromBuild ".make-app-stamp")).Trim() } else { "" }
    if ($FromBuild -and $stamp -and $fromStamp -and ((& $key $fromStamp) -eq (& $key $stamp))) {
        $ToolsDir = $FromBuild
        Say "the release build in $FromBuild ($(($fromStamp -split "`n")[0]), these arguments): nothing built"
    } else {
        if ($FromBuild) { Say "-FromBuild $FromBuild is not this tree's build with these arguments: building" }
        $stampFile = Join-Path $BuildDir ".make-app-stamp"
        if (Test-Path $stampFile) { Remove-Item -Force $stampFile }
        # The configure is run when the build folder's last one had other arguments (or none, or -Reconfigure):
        # with the same arguments ninja runs CMake again itself when any of its inputs changed (the CMakeLists,
        # the globbed folders, each CMAKE_CONFIGURE_DEPENDS file), as in every build of the tree; an unneeded one
        # took 250 s of a 270 s rerun with nothing to build on Windows (game.cmake's generation: winbuild1).
        $configuredFile = Join-Path $BuildDir ".make-windows-configured"
        $configureKey = ($cmakeArgs -join "`n")
        $configured = (Test-Path (Join-Path $BuildDir "build.ninja")) -and (Test-Path $configuredFile) -and
                      ((Get-Content -Raw $configuredFile) -ceq $configureKey)
        if ($Reconfigure -or -not $configured) {
            if (Test-Path $configuredFile) { Remove-Item -Force $configuredFile }
            $clock = [System.Diagnostics.Stopwatch]::StartNew()
            & cmake @cmakeArgs | Out-Null
            if ($LASTEXITCODE -ne 0) { Die "cmake failed" }
            Set-Content -NoNewline -Path $configuredFile -Value $configureKey
            Say "configured ($(Took $clock))"
        } else { Say "configured before with these arguments: ninja configures again if an input changed (-Reconfigure: always)" }
        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        & ninja -C $BuildDir ps2EntryRunner
        if ($LASTEXITCODE -ne 0) { Die "build failed" }
        Say "built ($(Took $clock))"
        if ($stamp) { Set-Content -NoNewline -Path $stampFile -Value $stamp }
    }
}
$Runner = Join-Path $ToolsDir "ps2xRuntime\ps2EntryRunner.exe"
if (-not (Test-Path $Runner)) { Die "no runner at $Runner" }
# Tool NAME: a build tool the folder's steps run, from ToolsDir (built there when it is BuildDir).
function Tool([string]$name) {
    if ($ToolsDir -eq $BuildDir) {
        & ninja -C $BuildDir $name | Out-Null
        if ($LASTEXITCODE -ne 0) { Die "$name build failed" }
    }
    $exe = Join-Path $ToolsDir "ps2xRuntime\$name.exe"
    if (-not (Test-Path $exe)) { Die "no $name in $ToolsDir" }
    return $exe
}

# ---- Xbox skaters (optional): the Xbox release's more detailed skaters made
# into the PS2's model format here (ps2xNativeAssets --xbox-models) under the
# pack's models\; the app only loads them (Settings > Display > Xbox Skaters).
if (-not $NoGameData -and (Test-Path $XboxDir) -and (Test-Path $XboxPack)) {
    $ps2xNativeAssetsExe = Tool ps2xNativeAssets
    $models = Join-Path $XboxPack "models"
    if (Test-Path $models) { Remove-Item -Recurse -Force $models }
    & $ps2xNativeAssetsExe --xbox-models $XboxPack --ps2 $GameFiles --xbox $XboxDir | ForEach-Object { Say $_ }
    if ($LASTEXITCODE -ne 0) { Die "ps2xNativeAssets --xbox-models failed" }
}

# ---- the Zoo and the Ape (optional): the Xbox release's Zoo (level, split
# screen, ambient NPCs) and Ape made into the PS2's formats here, with the Xbox
# originals of their textures (ps2xXboxConvert); the app only loads them
# (xbox-converted\). Without the Xbox files the Zoo is the PS2 disc's prototype.
$XboxConverted = Join-Path $Dist ".xbox-converted"
if (Test-Path $XboxConverted) { Remove-Item -Recurse -Force $XboxConverted }
if (Test-Path $XboxDir) {
    $ps2xXboxConvertExe = Tool ps2xXboxConvert
    & $ps2xXboxConvertExe --xbox $XboxDir --ps2 $GameFiles --out $XboxConverted | ForEach-Object { Say $_ }
    if ($LASTEXITCODE -ne 0) { Die "ps2xXboxConvert could not make the Zoo and the Ape from $XboxDir" }
}

# ---- Estate (optional): TUT/Estate, an earlier Tutorial left on the GameCube
# disc, extracted from your GameCube image (tools\gc_disc.py) and made into the
# files the game loads (ps2xGcConvert); the app only loads them (estate\).
$Estate = Join-Path $Dist ".estate"
if (Test-Path $Estate) { Remove-Item -Recurse -Force $Estate }
if ($GcImage -and -not $NoGameData) {
    if (-not (Test-Path $GcImage)) { Die "no GameCube image $GcImage" }
    if (-not $python) { Die "Python 3 is needed for the GameCube image (or leave out -GcImage)" }
    $gcCache = Join-Path $Dist ".gc-cache"
    if (Test-Path $gcCache) { Remove-Item -Recurse -Force $gcCache }
    $gcArgs = PythonArgs @("-I", (Join-Path $Root "tools\gc_disc.py"), "extract", $GcImage, "--out", $gcCache, "assets/levels/TUT/Estate")
    & $python.Source @gcArgs | ForEach-Object { Say $_ }
    if ($LASTEXITCODE -ne 0) { Die "could not extract TUT/Estate from the GameCube image $GcImage" }
    $ps2xGcConvertExe = Tool ps2xGcConvert
    & $ps2xGcConvertExe --gc $gcCache --out $Estate | ForEach-Object { Say $_ }
    if ($LASTEXITCODE -ne 0) { Die "ps2xGcConvert could not make Estate from $GcImage" }
}

# ---- the folder ----------------------------------------------------------------------
Say "assembling $AppDir"
$clock = [System.Diagnostics.Stopwatch]::StartNew()
if (Test-Path $AppDir) { Remove-Item -Recurse -Force $AppDir }
New-Item -ItemType Directory -Force $AppDir | Out-Null
Copy-Item $Runner (Join-Path $AppDir "$AppName.exe")
# The app's own font (no game data): Michroma (runtime\resources\fonts, SIL Open Font License 1.1: the TTF
# unchanged, its licence beside it), the collection menu's text before Aggressive Inline's disc is set up.
$fontsOut = Join-Path $AppDir "fonts"
New-Item -ItemType Directory -Force $fontsOut | Out-Null
Copy-Item (Join-Path $PS2Recomp "runtime\resources\fonts\Michroma-Regular.ttf"), (Join-Path $PS2Recomp "runtime\resources\fonts\Michroma-OFL.txt") $fontsOut
# Game files: hard links where the volume allows it (no extra space), else copies.
# A collection: each game's files in games\<id>.
$GameOut = if ($Collection) { Join-Path $AppDir "games\ai" } else { $AppDir }
$discOut = Join-Path $GameOut "disc"
$linked = 0
if (-not $NoGameData) {
Get-ChildItem -Recurse -File $GameFiles | Where-Object { $_.Name -ne ".stamp" } | ForEach-Object {
    $rel = $_.FullName.Substring($GameFiles.Length).TrimStart('\')
    $dest = Join-Path $discOut $rel
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    try { New-Item -ItemType HardLink -Path $dest -Target $_.FullName -ErrorAction Stop | Out-Null; $linked++ }
    catch { Copy-Item $_.FullName $dest }
}
if (Test-Path $XboxFiles) {
    # The Xbox files the app reads (the Zoo): xbox\ next to disc\.
    $filesOut = Join-Path $GameOut "xbox"
    Get-ChildItem -Recurse -File $XboxFiles | ForEach-Object {
        $rel = $_.FullName.Substring($XboxFiles.Length).TrimStart('\')
        $dest = Join-Path $filesOut $rel
        New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
        Copy-Item $_.FullName $dest
    }
    Say "Xbox files the app reads in xbox\"
}
if (Test-Path $XboxConverted) {
    # The Zoo and the Ape made from the Xbox files: xbox-converted\ next to disc\.
    $convertedOut = Join-Path $GameOut "xbox-converted"
    Get-ChildItem -Recurse -File $XboxConverted | ForEach-Object {
        $rel = $_.FullName.Substring($XboxConverted.Length).TrimStart('\')
        $dest = Join-Path $convertedOut $rel
        New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
        try { New-Item -ItemType HardLink -Path $dest -Target $_.FullName -ErrorAction Stop | Out-Null }
        catch { Copy-Item $_.FullName $dest }
    }
    Say "the Zoo and the Ape made from the Xbox files in xbox-converted\"
}
if (Test-Path $Estate) {
    # Estate for Free Skate: estate\ next to disc\.
    $estateOut = Join-Path $GameOut "estate"
    New-Item -ItemType Directory -Force $estateOut | Out-Null
    Get-ChildItem -File $Estate | ForEach-Object { Copy-Item $_.FullName (Join-Path $estateOut $_.Name) }
    Say "Estate (the GameCube disc's earlier Tutorial) for Free Skate in estate\"
}
if (Test-Path $XboxPack) {
    $packOut = Join-Path $GameOut "xbox-pack"
    Get-ChildItem -Recurse -File $XboxPack | Where-Object { $_.Name -ne ".stamp" } | ForEach-Object {
        $rel = $_.FullName.Substring($XboxPack.Length).TrimStart('\')
        $dest = Join-Path $packOut $rel
        New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
        try { New-Item -ItemType HardLink -Path $dest -Target $_.FullName -ErrorAction Stop | Out-Null }
        catch { Copy-Item $_.FullName $dest }
    }
    Say "Xbox textures and movies in xbox-pack\"
}
if ($Collection -and ($Games -contains "dm2")) {
    $dm2Out = Join-Path $AppDir "games\dm2\disc"
    Get-ChildItem -Recurse -File $Dm2DiscDir | ForEach-Object {
        $rel = $_.FullName.Substring((Resolve-Path $Dm2DiscDir).Path.Length).TrimStart('\')
        $dest = Join-Path $dm2Out $rel
        New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
        try { New-Item -ItemType HardLink -Path $dest -Target $_.FullName -ErrorAction Stop | Out-Null }
        catch { Copy-Item $_.FullName $dest }
    }
    Say "DM2 game files in games\dm2\disc"
    # DM2's Xbox-only levels (Venice, Greenville) for its Free Ride
    # (tools\dm2_xbox.py, Python 3), made from -Dm2XboxIso here,
    # their textures within dm2_xbox.py's default texture budget (DM2's
    # texture cache is this game's: it does not check that a frame's
    # textures fit GS memory); the app never converts.
    if ($python -and $Dm2XboxIso) {
        $dm2XboxCache = Join-Path $Dist ".dm2-xbox-cache"
        $xArgs = PythonArgs @("-I", (Join-Path $Root "tools\xbox_files.py"), "extract", "--iso", $Dm2XboxIso, "--cache", $dm2XboxCache)
        & $python.Source @xArgs | ForEach-Object { Say $_ }
        if ($LASTEXITCODE -ne 0) { Die "could not extract the DM2 Xbox image $Dm2XboxIso" }
        $xboxLevels = Join-Path $Dist ".dm2-xbox-levels"
        if (Test-Path $xboxLevels) { Remove-Item -Recurse -Force $xboxLevels }
        # With them: the park editor's Desert and Highway (with the PS2 disc's
        # Beach theme's light points), the attract demo's replays the PS2
        # disc lacks and the gallery's Retro movie (with
        # ffmpeg, if there is one), each behind its own setting.
        $xlArgs = PythonArgs @("-I", (Join-Path $Root "tools\dm2_xbox.py"), "convert", "--xbox", $dm2XboxCache, "--ps2", $Dm2DiscDir, "--out", $xboxLevels)
        & $python.Source @xlArgs | ForEach-Object { Say $_ }
        if ($LASTEXITCODE -ne 0) { Die "dm2_xbox.py could not make DM2's Xbox levels from $Dm2XboxIso" }
        Copy-Item -Recurse $xboxLevels (Join-Path $AppDir "games\dm2\xbox-levels")
        Say "DM2's Xbox levels for its Free Ride in games\dm2\xbox-levels"
    } elseif ($python) { Say "no -Dm2XboxIso: no Venice and Greenville in DM2" }
    # DM2's Xbox textures: its levels' from -Dm2XboxIso, made as the extras' setup makes them (ps2xExtras
    # xbox-pack --no-movies; collection/extras/setup.cpp), into games\dm2\xbox-pack, where the app reads a
    # bundled game's pack (Xbox Textures).
    if ($Dm2XboxIso -and (Test-Path $Dm2XboxIso)) {
        $ps2xExtrasExe = Tool ps2xExtras
        $dm2Pack = Join-Path $Dist ".dm2-xbox-pack"
        if (Test-Path $dm2Pack) { Remove-Item -Recurse -Force $dm2Pack }
        & $ps2xExtrasExe xbox-pack --ps2 (Resolve-Path $Dm2DiscDir).Path --xbox $Dm2XboxIso --out $dm2Pack --no-movies | ForEach-Object { Say $_ }
        if ($LASTEXITCODE -ne 0) { Die "ps2xExtras xbox-pack could not make DM2's Xbox textures from $Dm2XboxIso" }
        Copy-Item -Recurse $dm2Pack (Join-Path $AppDir "games\dm2\xbox-pack")
        Say "DM2's Xbox textures in games\dm2\xbox-pack"
    }
}
if ($Collection -and ($Games -contains "bmx")) {
    $bmxOut = Join-Path $AppDir "games\bmx\disc"
    Get-ChildItem -Recurse -File $BmxDiscDir | ForEach-Object {
        $rel = $_.FullName.Substring((Resolve-Path $BmxDiscDir).Path.Length).TrimStart('\')
        $dest = Join-Path $bmxOut $rel
        New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
        try { New-Item -ItemType HardLink -Path $dest -Target $_.FullName -ErrorAction Stop | Out-Null }
        catch { Copy-Item $_.FullName $dest }
    }
    Say "BMX XXX game files in games\bmx\disc"
    # BMX XXX's Xbox and GameCube releases' content made into its PS2 formats
    # (tools\bmx_releases.py, as tools/make-app.sh; the app never reads the
    # images): their challenge scripts, the fuller French, German and Spanish
    # string tables, the level select's preview movies and the Z-Axis logo
    # movie (with ffmpeg), the GameCube's Secret Level and three test
    # levels, each behind its own setting.
    if ($python -and (($BmxXboxIso -and (Test-Path $BmxXboxIso)) -or ($BmxGcImage -and (Test-Path $BmxGcImage)))) {
        $bmxReleases = Join-Path $Dist ".bmx-releases"
        $relArgs = @("--ps2", (Resolve-Path $BmxDiscDir).Path, "--out", $bmxReleases)
        if ($BmxXboxIso -and (Test-Path $BmxXboxIso)) {
            $bmxXboxCache = Join-Path $Dist ".bmx-xbox-cache"
            $xArgs = PythonArgs @("-I", (Join-Path $Root "tools\xbox_files.py"), "extract", "--iso", $BmxXboxIso, "--cache", $bmxXboxCache)
            & $python.Source @xArgs | ForEach-Object { Say $_ }
            if ($LASTEXITCODE -ne 0) { Die "could not extract the BMX XXX Xbox image $BmxXboxIso" }
            $relArgs += @("--xbox", $bmxXboxCache)
        }
        if ($BmxGcImage -and (Test-Path $BmxGcImage)) {
            $bmxGcCache = Join-Path $Dist ".bmx-gc-cache"
            $gcItem = Get-Item $BmxGcImage
            # (the files taken are in the stamp: a cache of fewer is read again)
            $gcFiles = @("assets/string.zal", "assets/editor/object.obj", "assets/levels/*/objctv.txt", "assets/levels/cmp_scrt",
                "assets/levels/tst_flat", "assets/levels/tst_grnd", "assets/levels/tst_vert")
            $gcStamp = "$($gcItem.Length) $($gcItem.LastWriteTimeUtc.Ticks) $($gcItem.Name) $($gcFiles -join ' ')"
            $gcStampFile = Join-Path $bmxGcCache ".stamp"
            if (-not (Test-Path $gcStampFile) -or (Get-Content -Raw $gcStampFile).Trim() -ne $gcStamp) {
                if (Test-Path $bmxGcCache) { Remove-Item -Recurse -Force $bmxGcCache }
                $gArgs = PythonArgs (@("-I", (Join-Path $Root "tools\gc_disc.py"), "extract", $BmxGcImage, "--out", $bmxGcCache) + $gcFiles)
                & $python.Source @gArgs | ForEach-Object { Say $_ }
                if ($LASTEXITCODE -ne 0) { Die "could not read the BMX XXX GameCube image $BmxGcImage" }
                Set-Content -NoNewline -Path $gcStampFile -Value $gcStamp
            }
            $relArgs += @("--gc", $bmxGcCache)
        }
        if (Test-Path $bmxReleases) { Remove-Item -Recurse -Force $bmxReleases }
        $rArgs = PythonArgs (@("-I", (Join-Path $Root "tools\bmx_releases.py"), "convert") + $relArgs)
        & $python.Source @rArgs | ForEach-Object { Say $_ }
        if ($LASTEXITCODE -ne 0) { Die "bmx_releases.py could not make BMX XXX's other releases' content" }
        Copy-Item -Recurse $bmxReleases (Join-Path $AppDir "games\bmx\releases")
        Say "BMX XXX's Xbox and GameCube content in games\bmx\releases"
    } else { Say "no -BmxXboxIso or -BmxGcImage: BMX XXX as its PS2 release alone" }
    # BMX XXX's Xbox textures: its levels' and their props' (the ZAL archives) from -BmxXboxIso, made as the
    # extras' setup makes them (ps2xExtras xbox-pack --no-movies --archives), into games\bmx\xbox-pack, where the
    # app reads a bundled game's pack (Xbox Textures).
    if ($BmxXboxIso -and (Test-Path $BmxXboxIso)) {
        $ps2xExtrasExe = Tool ps2xExtras
        $bmxPack = Join-Path $Dist ".bmx-xbox-pack"
        if (Test-Path $bmxPack) { Remove-Item -Recurse -Force $bmxPack }
        & $ps2xExtrasExe xbox-pack --ps2 (Resolve-Path $BmxDiscDir).Path --xbox $BmxXboxIso --out $bmxPack --no-movies --archives | ForEach-Object { Say $_ }
        if ($LASTEXITCODE -ne 0) { Die "ps2xExtras xbox-pack could not make BMX XXX's Xbox textures from $BmxXboxIso" }
        Copy-Item -Recurse $bmxPack (Join-Path $AppDir "games\bmx\xbox-pack")
        Say "BMX XXX's Xbox textures in games\bmx\xbox-pack"
    }
}
}  # (-NoGameData)
$lgpl = Get-ChildItem -Path (Join-Path $Root "third_party-cache") -Filter "COPYING.LGPLv2.1" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
$aboutData = if ($NoGameData) {
    "It holds no game data: at first start it asks for each game's PS2 disc`r`n(an ISO, a BIN/CUE dump or the disc's folder) and copies its files into`r`n%APPDATA%\$AppName."
} else { "with the game files of your own disc`r`n(disc\). Do not share this folder: it holds game data." }
@"
$AppName $AppVersion for Windows x64: a native port of the PlayStation 2 game,
statically recompiled with PS2Recomp, $aboutData

Settings, controls, saves, mods and logs: %APPDATA%\$AppName
Full screen: Alt+Enter or F11. Settings: the game's own menus
(Options > Settings, or Settings in the pause menu).

FFmpeg (LGPL 2.1) is linked statically for the game's movies; its licence is
in FFMPEG-LICENSE.txt. SDL3 is under the zlib licence. The menu's font,
Michroma, is under the SIL Open Font License 1.1 (fonts\Michroma-OFL.txt).
"@ | Set-Content -Path (Join-Path $AppDir "README.txt")
if ($lgpl) { Copy-Item $lgpl.FullName (Join-Path $AppDir "FFMPEG-LICENSE.txt") }
$size = (Get-ChildItem -Recurse -File $AppDir | Measure-Object -Property Length -Sum).Sum / 1MB
Say ("done: $AppDir ({0:N0} MB, {1} game files hard-linked, {2})" -f $size, $linked, (Took $clock))
if ($Zip) {
    $zipPath = Join-Path $Dist "$AppName.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath }
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    # (each entry named here with "/": Windows PowerShell 5.1's Compress-Archive, and ZipFile.CreateFromDirectory in a
    # process that targets a .NET Framework before 4.6.1, such as powershell.exe, write "Z-Axis Collection\README.txt",
    # against the zip format: unzip tools outside Windows warn, or make one file of that name: winbuild1)
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zipStream = [System.IO.File]::Open($zipPath, [System.IO.FileMode]::CreateNew)
    $archive = New-Object System.IO.Compression.ZipArchive($zipStream, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem -Recurse -File $AppDir) {
            $entry = "$AppName/" + $f.FullName.Substring($AppDir.Length).TrimStart('\').Replace('\', '/')
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry,
                [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $archive.Dispose(); $zipStream.Dispose() }
    Say "zip: $zipPath ($(Took $clock))"
}
exit 0
