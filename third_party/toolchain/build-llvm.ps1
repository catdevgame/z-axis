# build-llvm.ps1 -Out DIR [-Jobs 8]: the app's bundled compiler (llvm.lock) for windows-x64, as build-llvm.sh makes it
# for the other hosts, but as two programs, clang.exe and lld.exe (and their names: clang++, lld-link), the AArch64 and X86 targets alone, no zlib/zstd/libxml2, the static C runtime (/MT); and clang's resource
# headers. Into DIR\<version>-windows-x64-<recipe>.tar.xz and its SHA-256. Needs a VS 2022 x64 developer environment
# (cl or clang-cl, CMake, Ninja), run at below-normal priority.
param([Parameter(Mandatory = $true)][string]$Out, [int]$Jobs = 8)
$ErrorActionPreference = 'Stop'
(Get-Process -Id $PID).PriorityClass = 'BelowNormal'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
function Lock([string]$k) { (Get-Content "$here\llvm.lock" | Where-Object { $_ -match "^$k " } | Select-Object -First 1) -replace "^$k ", '' }
$version = Lock 'version'; $url = Lock 'source'; $sha = Lock 'source_sha256'
New-Item -ItemType Directory -Force $Out | Out-Null
$tar = "$Out\llvm-project-$version.src.tar.xz"
if (-not (Test-Path $tar)) { Invoke-WebRequest -Uri $url -OutFile "$tar.part"; Move-Item "$tar.part" $tar }
$got = (Get-FileHash -Algorithm SHA256 $tar).Hash.ToLower()
if ($got -ne $sha) { throw "build-llvm.ps1: $tar is $got, llvm.lock pins $sha" }
$src = "$Out\llvm-project-$version.src"
if (-not (Test-Path $src)) { tar -xf $tar -C $Out }
$work = "$Out\work-windows-x64"
# What decides the programs: the configuration below. Its hash is the archive's <recipe>, so a change to the rest of
# this file (the link jobs, the packing) keeps the name of an archive that comes out the same.
$cfg = @('-DCMAKE_BUILD_TYPE=Release', '-DLLVM_ENABLE_PROJECTS=clang;lld', '-DLLVM_TARGETS_TO_BUILD=AArch64;X86',
  # (no multicall llvm.exe: LLVM's driver build makes an empty tool list on Windows; clang and lld apart)
  '-DLLVM_DISTRIBUTION_COMPONENTS=clang;lld;clang-resource-headers',
  '-DLLVM_ENABLE_ZLIB=OFF', '-DLLVM_ENABLE_ZSTD=OFF', '-DLLVM_ENABLE_LIBXML2=OFF', '-DLLVM_ENABLE_ASSERTIONS=OFF',
  '-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_BENCHMARKS=OFF', '-DLLVM_INCLUDE_EXAMPLES=OFF', '-DLLVM_INCLUDE_DOCS=OFF',
  '-DLLVM_ENABLE_BINDINGS=OFF', '-DCLANG_ENABLE_ARCMT=OFF', '-DCLANG_ENABLE_STATIC_ANALYZER=OFF', '-DCLANG_PLUGIN_SUPPORT=OFF',
  '-DLLVM_ENABLE_PLUGINS=OFF', '-DLLVM_ENABLE_DIA_SDK=OFF', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded', '-DCMAKE_C_COMPILER=clang-cl', '-DCMAKE_CXX_COMPILER=clang-cl',
  '-DLLVM_USE_LINKER=lld')
$sha256 = [Security.Cryptography.SHA256]::Create()
$recipe = (-join ($sha256.ComputeHash([Text.Encoding]::UTF8.GetBytes("windows-x64`n" + ($cfg -join "`n") + "`n")) |
  ForEach-Object { $_.ToString('x2') })).Substring(0, 8)
$args = @('-G', 'Ninja', '-S', "$src\llvm", '-B', "$work\build", "-DCMAKE_INSTALL_PREFIX=$work\install") + $cfg + @('-DLLVM_PARALLEL_LINK_JOBS=2')
& cmake @args
if ($LASTEXITCODE) { throw "cmake exit $LASTEXITCODE" }
& ninja -C "$work\build" -j $Jobs install-distribution-stripped
if ($LASTEXITCODE) { throw "ninja exit $LASTEXITCODE" }
$name = "$version-windows-x64-$recipe"
$pkg = "$work\$name"
if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
New-Item -ItemType Directory -Force $pkg | Out-Null
Copy-Item -Recurse "$work\install\bin", "$work\install\lib" $pkg
Copy-Item "$src\llvm\LICENSE.TXT" "$pkg\LICENSE.TXT"
tar -C $work -cJf "$Out\$name.tar.xz" $name
$h = (Get-FileHash -Algorithm SHA256 "$Out\$name.tar.xz").Hash.ToLower()
# LF only, so shasum -c and sha256sum -c read it on the Mac and Linux.
[IO.File]::WriteAllText("$Out\$name.tar.xz.sha256", "$h  $name.tar.xz`n")
"$name.tar.xz $h $((Get-Item "$Out\$name.tar.xz").Length) bytes"
